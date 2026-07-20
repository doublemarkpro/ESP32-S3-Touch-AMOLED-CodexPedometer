from __future__ import annotations

import datetime as dt
import json
import os
import queue
import shutil
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parent
USAGE_FILE = ROOT / "codex_usage.json"
CACHE_TTL_SECONDS = 30
CODEX_TIMEOUT_SECONDS = 12

_cache_lock = threading.Lock()
_cache_payload: dict[str, Any] | None = None
_cache_time = 0.0


def find_codex_exe() -> str | None:
    env_path = os.environ.get("CODEX_EXE")
    if env_path and Path(env_path).exists():
        return env_path

    candidates: list[Path] = []
    vscode_ext = Path.home() / ".vscode" / "extensions"
    if vscode_ext.exists():
        candidates.extend(vscode_ext.glob("openai.chatgpt-*/bin/windows-x86_64/codex.exe"))

    candidates = sorted(candidates, key=lambda p: p.stat().st_mtime, reverse=True)
    if candidates:
        return str(candidates[0])

    return shutil.which("codex")


def send_json(proc: subprocess.Popen[str], payload: dict[str, Any]) -> None:
    if proc.stdin is None:
        raise RuntimeError("codex app-server stdin is closed")
    proc.stdin.write(json.dumps(payload, separators=(",", ":")) + "\n")
    proc.stdin.flush()


def read_codex_responses(codex_exe: str) -> dict[int, dict[str, Any]]:
    proc = subprocess.Popen(
        [codex_exe, "app-server", "--stdio"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )
    lines: queue.Queue[str] = queue.Queue()

    def reader() -> None:
        assert proc.stdout is not None
        for line in proc.stdout:
            lines.put(line.rstrip("\r\n"))

    threading.Thread(target=reader, daemon=True).start()

    try:
        send_json(
            proc,
            {
                "id": 1,
                "method": "initialize",
                "params": {
                    "clientInfo": {
                        "name": "codex-pedometer-bridge",
                        "version": "0.2",
                    },
                    "capabilities": {"experimentalApi": True},
                },
            },
        )

        responses: dict[int, dict[str, Any]] = {}
        requested_account = False
        rate_seen_at: float | None = None
        deadline = time.monotonic() + CODEX_TIMEOUT_SECONDS
        while time.monotonic() < deadline:
            try:
                line = lines.get(timeout=0.25)
            except queue.Empty:
                continue

            try:
                message = json.loads(line)
            except json.JSONDecodeError:
                continue

            message_id = message.get("id")
            if isinstance(message_id, int):
                responses[message_id] = message
                if message_id == 2 and rate_seen_at is None:
                    rate_seen_at = time.monotonic()

            if not requested_account and 1 in responses:
                requested_account = True
                send_json(proc, {"method": "initialized"})
                send_json(proc, {"id": 2, "method": "account/rateLimits/read"})
                send_json(proc, {"id": 3, "method": "account/usage/read"})

            if 2 in responses and 3 in responses:
                return responses
            if rate_seen_at is not None and time.monotonic() - rate_seen_at > 2:
                return responses

        raise TimeoutError("timed out waiting for account/rateLimits/read")
    finally:
        try:
            proc.terminate()
        except Exception:
            pass


def pick_codex_limit(rate_payload: dict[str, Any]) -> dict[str, Any]:
    result = rate_payload.get("result") or {}
    by_id = result.get("rateLimitsByLimitId") or {}
    if isinstance(by_id, dict) and isinstance(by_id.get("codex"), dict):
        return by_id["codex"]

    snapshot = result.get("rateLimits")
    if isinstance(snapshot, dict):
        return snapshot

    raise RuntimeError("codex rate limit snapshot missing")


def format_reset_time(timestamp: int | None) -> str | None:
    if not timestamp:
        return None
    return dt.datetime.fromtimestamp(timestamp).astimezone().strftime("%m-%d %H:%M")


def build_real_payload() -> dict[str, Any]:
    codex_exe = find_codex_exe()
    if codex_exe is None:
        raise RuntimeError("codex.exe not found")

    responses = read_codex_responses(codex_exe)
    rate_message = responses.get(2)
    if not rate_message or "result" not in rate_message:
        raise RuntimeError("account/rateLimits/read failed")

    snapshot = pick_codex_limit(rate_message)
    primary = snapshot.get("primary") or {}
    used_percent = int(primary.get("usedPercent") or 0)
    used_percent = max(0, min(100, used_percent))
    reset_at = primary.get("resetsAt")

    result = rate_message.get("result") or {}
    credits = result.get("rateLimitResetCredits") or {}
    available_resets = credits.get("availableCount")

    usage_message = responses.get(3) or {}
    usage_result = usage_message.get("result") or {}
    daily_buckets = usage_result.get("dailyUsageBuckets") or []
    today = dt.date.today().isoformat()
    today_tokens = 0
    if isinstance(daily_buckets, list):
        for bucket in daily_buckets:
            if bucket.get("startDate") == today:
                today_tokens = int(bucket.get("tokens") or 0)
                break

    summary = usage_result.get("summary") or {}
    updated_parts = [f"real {used_percent}%"]
    reset_text = format_reset_time(int(reset_at)) if reset_at else None
    if reset_text:
        updated_parts.append(f"reset {reset_text}")

    return {
        "label": "Codex quota",
        "used_tokens": used_percent,
        "limit_tokens": 100,
        "updated": ", ".join(updated_parts),
        "source": "codex-app-server",
        "used_percent": used_percent,
        "reset_at": reset_at,
        "reset_at_local": reset_text,
        "reset_credits": available_resets,
        "today_tokens": today_tokens,
        "lifetime_tokens": summary.get("lifetimeTokens"),
        "plan_type": snapshot.get("planType"),
        "limit_id": snapshot.get("limitId"),
    }


def load_manual_payload(reason: str) -> dict[str, Any]:
    try:
        payload = json.loads(USAGE_FILE.read_text(encoding="utf-8"))
    except FileNotFoundError:
        payload = {
            "label": "Codex quota",
            "used_tokens": 0,
            "limit_tokens": 100,
            "updated": "missing codex_usage.json",
        }

    payload.setdefault("label", "Codex quota")
    payload.setdefault("used_tokens", 0)
    payload.setdefault("limit_tokens", 100)
    payload["source"] = "manual-fallback"
    payload["fallback_reason"] = reason[:160]
    if payload.get("updated") in (None, "", "manual"):
        payload["updated"] = "fallback manual"
    return payload


def get_usage_payload() -> dict[str, Any]:
    global _cache_payload, _cache_time

    now = time.monotonic()
    with _cache_lock:
        if _cache_payload is not None and now - _cache_time < CACHE_TTL_SECONDS:
            return dict(_cache_payload)

    try:
        payload = build_real_payload()
    except Exception as exc:
        payload = load_manual_payload(str(exc))

    with _cache_lock:
        _cache_payload = dict(payload)
        _cache_time = time.monotonic()

    return payload


class Handler(BaseHTTPRequestHandler):
    def do_GET(self) -> None:
        if self.path not in ("/", "/usage"):
            self.send_error(404, "Not found")
            return

        payload = get_usage_payload()
        data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, fmt: str, *args: object) -> None:
        print("%s - %s" % (self.address_string(), fmt % args))


def main() -> None:
    host = "0.0.0.0"
    port = 8765
    server = ThreadingHTTPServer((host, port), Handler)
    print(f"Serving Codex usage bridge at http://{host}:{port}/usage")
    print("Source: Codex app-server account/rateLimits/read; fallback: codex_usage.json.")
    server.serve_forever()


if __name__ == "__main__":
    main()
