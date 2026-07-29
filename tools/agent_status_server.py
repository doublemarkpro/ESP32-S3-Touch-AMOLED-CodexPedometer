"""Local AI agent status bridge for the ESP32-S3 watch face.

Claude Code and Codex hooks POST lifecycle events here; the watch polls
GET /status and paints its ring green / yellow / amber / red, replacing a
Bluetooth status lamp with a Wi-Fi one.

Run:      python tools/agent_status_server.py
Hooks:    python tools/agent_status_server.py --print-hooks
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import threading
import time
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any

HOST = "0.0.0.0"
PORT = 8766

# A turn that stops emitting events is treated as finished, so a crashed or
# force-quit agent cannot leave the ring stuck on yellow forever.
WORKING_TIMEOUT_SECONDS = 180.0
# How long a finished turn keeps showing "done" before falling back to idle.
DONE_LINGER_SECONDS = 90.0

STATES = ("idle", "working", "waiting", "error", "done")
# Higher wins when Codex and Claude disagree, so the ring always shows the
# state that most wants your attention.
PRIORITY = {"error": 4, "waiting": 3, "working": 2, "done": 1, "idle": 0}

_lock = threading.Lock()
_agents: dict[str, dict[str, Any]] = {}


def _now() -> float:
    return time.monotonic()


def _blank(name: str) -> dict[str, Any]:
    return {
        "name": name,
        "state": "idle",
        "project": "",
        "detail": "",
        "since": _now(),
        "updated": _now(),
        # Wall clock of the last state *change*, so the watch shows when the
        # state last moved rather than a clock that ticks on every poll.
        "changed_wall": time.time(),
    }


def _effective(agent: dict[str, Any]) -> str:
    """Apply the staleness rules to one agent's raw state."""
    state = agent["state"]
    age = _now() - agent["updated"]
    if state in ("working", "waiting") and age > WORKING_TIMEOUT_SECONDS:
        return "idle"
    if state == "done" and age > DONE_LINGER_SECONDS:
        return "idle"
    return state


def record_event(
    name: str, state: str, project: str = "", detail: str = "", note: str = ""
) -> None:
    name = (name or "agent").strip().lower()
    state = (state or "").strip().lower()
    if state not in STATES:
        state = "working"

    # One line per incoming hook: without this there is no way to tell a hook
    # that never fired from one that fired with the wrong state.
    print(
        f"{time.strftime('%H:%M:%S')}  {name:<7} -> {state:<8}"
        f" project={project or '-':<24} {('[' + detail + ']') if detail else ''}"
        f"{(' <' + note + '>') if note else ''}",
        flush=True,
    )

    with _lock:
        agent = _agents.get(name) or _blank(name)
        if agent["state"] != state:
            agent["since"] = _now()
            agent["changed_wall"] = time.time()
        agent["state"] = state
        agent["updated"] = _now()
        if project:
            agent["project"] = project[:38]
        if detail:
            agent["detail"] = detail[:46]
        _agents[name] = agent


def build_status() -> dict[str, Any]:
    with _lock:
        snapshot = {name: dict(agent) for name, agent in _agents.items()}

    per_agent = {name: _effective(agent) for name, agent in snapshot.items()}

    lead_name = ""
    lead_state = "idle"
    for name, state in per_agent.items():
        if not lead_name or PRIORITY[state] > PRIORITY[lead_state]:
            lead_name, lead_state = name, state

    lead = snapshot.get(lead_name, {})
    elapsed = int(_now() - lead["since"]) if lead else 0
    # A state we aged out of is no longer "since" what the agent reported.
    if lead and per_agent.get(lead_name) != lead.get("state"):
        elapsed = int(_now() - lead["updated"])

    return {
        "state": lead_state,
        "agent": {"claude": "Claude", "codex": "Codex"}.get(lead_name, lead_name.title()),
        "project": lead.get("project", ""),
        "detail": lead.get("detail", ""),
        "elapsed": max(0, elapsed),
        "codex": per_agent.get("codex", "idle" if "codex" in per_agent else ""),
        "claude": per_agent.get("claude", "idle" if "claude" in per_agent else ""),
        "updated": time.strftime("%H:%M:%S", time.localtime(lead["changed_wall"]))
        if lead
        else "",
    }


class BridgeServer(ThreadingHTTPServer):
    """HTTPServer enables SO_REUSEADDR, which on Windows lets a second copy
    bind a port that is already being listened on. Both instances then
    receive events at random, which looks exactly like flaky hooks. Refuse
    the second bind so a duplicate fails loudly instead."""

    allow_reuse_address = False


WEATHER_CACHE_SECONDS = 600
QUOTE_CACHE_SECONDS = 20
CLAUDE_CACHE_SECONDS = 60
CLAUDE_WINDOW_HOURS = 5.0

# Same cities the firmware knows, with the coordinates it uses. Open-Meteo
# needs no key, which is the whole point: the watch has an AMap key in its
# NVS, and copying it onto the PC as well is one more secret to lose.
CITIES = {
    "青岛": (36.06488, 120.38042), "上海": (31.23040, 121.47370),
    "北京": (39.90420, 116.40740), "深圳": (22.54310, 114.05790),
    "广州": (23.12910, 113.26440), "杭州": (30.27410, 120.15510),
    "南京": (32.06030, 118.79690), "济南": (36.65120, 117.12010),
}
# WMO weather codes, collapsed to the words the watch shows.
WMO = {
    0: "晴", 1: "多云", 2: "多云", 3: "阴", 45: "雾", 48: "雾",
    51: "小雨", 53: "小雨", 55: "中雨", 61: "小雨", 63: "中雨", 65: "大雨",
    66: "冻雨", 67: "冻雨", 71: "小雪", 73: "中雪", 75: "大雪", 77: "阵雪",
    80: "阵雨", 81: "阵雨", 82: "暴雨", 85: "阵雪", 86: "阵雪",
    95: "雷阵雨", 96: "雷阵雨", 99: "雷阵雨",
}

_weather_lock = threading.Lock()
_weather_cache: dict[str, tuple[float, dict[str, Any]]] = {}


def _http_get(url: str, timeout: float = 6.0) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": "codex-bridge/1"})
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return response.read()


def fetch_weather(city: str) -> dict[str, Any]:
    """Current conditions and today's range. Cached for ten minutes - the
    forecast does not change faster than that and neither does the widget."""
    city = city or "青岛"
    now = time.time()
    with _weather_lock:
        cached = _weather_cache.get(city)
        if cached and now - cached[0] < WEATHER_CACHE_SECONDS:
            return cached[1]

    latitude, longitude = CITIES.get(city, CITIES["青岛"])
    url = (
        "https://api.open-meteo.com/v1/forecast"
        f"?latitude={latitude}&longitude={longitude}"
        "&current=temperature_2m,weather_code"
        "&daily=temperature_2m_max,temperature_2m_min"
        "&timezone=auto&forecast_days=1"
    )
    try:
        payload = json.loads(_http_get(url).decode("utf-8"))
        current = payload["current"]
        daily = payload["daily"]
        result = {
            "city": city,
            "temp": round(float(current["temperature_2m"])),
            "condition": WMO.get(int(current["weather_code"]), "--"),
            "low": round(float(daily["temperature_2m_min"][0])),
            "high": round(float(daily["temperature_2m_max"][0])),
            "updated": time.strftime("%H:%M"),
        }
    except Exception as exc:  # noqa: BLE001 - any failure is just "no weather"
        result = {"city": city, "error": str(exc)[:80]}

    with _weather_lock:
        _weather_cache[city] = (now, result)
    return result


# ------------------------------------------------------------------ quotes
_quote_lock = threading.Lock()
_quote_cache: dict[str, tuple[float, dict[str, Any]]] = {}


def fetch_quote(code: str) -> dict[str, Any]:
    """Live quote from the same feed the watch uses. The feed speaks GBK,
    which is why the watch cannot read the name out of it and this can."""
    now = time.time()
    with _quote_lock:
        cached = _quote_cache.get(code)
        if cached and now - cached[0] < QUOTE_CACHE_SECONDS:
            return cached[1]

    try:
        raw = _http_get(f"http://qt.gtimg.cn/q={code}", timeout=4.0)
        text = raw.decode("gbk", errors="replace")
        fields = text.split('="', 1)[1].rstrip('";\n').split("~")
        price = float(fields[3])
        prev = float(fields[4])
        result = {
            "code": code,
            "name": fields[1],
            "price": price,
            "change": price - prev,
            "change_pct": (price - prev) / prev * 100.0 if prev else 0.0,
        }
    except Exception as exc:  # noqa: BLE001
        result = {"code": code, "error": str(exc)[:80]}

    with _quote_lock:
        _quote_cache[code] = (now, result)
    return result


# ----------------------------------------------------- Claude Code token use
_claude_lock = threading.Lock()
_claude_cache: tuple[float, dict[str, Any]] = (0.0, {})
_claude_offsets: dict[str, tuple[int, list[tuple[float, int]]]] = {}
_TS_RE = re.compile(r'"timestamp":"([^"]+)"')
_USAGE_RE = re.compile(r'"usage":\{([^}]*)\}')
_TOKEN_RE = re.compile(r'"(input_tokens|output_tokens|cache_creation_input_tokens|'
                       r'cache_read_input_tokens)":(\d+)')


def _parse_iso(value: str) -> float:
    try:
        from datetime import datetime
        return datetime.fromisoformat(value.replace("Z", "+00:00")).timestamp()
    except Exception:  # noqa: BLE001
        return 0.0


def claude_usage() -> dict[str, Any]:
    """Tokens Claude Code has spent recently, summed from its own transcripts.

    There is no local record of the plan's rate-limit percentage - that lives
    server-side behind the account's credentials - so this reports what can
    honestly be measured here: tokens and turns in a rolling window.

    Transcripts are append-only, so each file is read from where the last scan
    stopped rather than re-parsed; a long session is tens of megabytes.
    """
    global _claude_cache
    now = time.time()
    with _claude_lock:
        if now - _claude_cache[0] < CLAUDE_CACHE_SECONDS and _claude_cache[1]:
            return _claude_cache[1]

    root = Path.home() / ".claude" / "projects"
    cutoff = now - CLAUDE_WINDOW_HOURS * 3600.0
    day_start = time.mktime(time.localtime(now)[:3] + (0, 0, 0, 0, 0, -1))

    if root.is_dir():
        for path in root.rglob("*.jsonl"):
            try:
                stat = path.stat()
            except OSError:
                continue
            if stat.st_mtime < min(cutoff, day_start):
                continue

            key = str(path)
            offset, samples = _claude_offsets.get(key, (0, []))
            if stat.st_size < offset:      # rotated or truncated
                offset, samples = 0, []
            if stat.st_size > offset:
                try:
                    with path.open("r", encoding="utf-8", errors="replace") as fh:
                        fh.seek(offset)
                        for line in fh:
                            usage = _USAGE_RE.search(line)
                            if not usage:
                                continue
                            stamp = _TS_RE.search(line)
                            when = _parse_iso(stamp.group(1)) if stamp else now
                            counts = {key: int(value) for key, value
                                      in _TOKEN_RE.findall(usage.group(1))}
                            fresh = (counts.get("input_tokens", 0)
                                     + counts.get("output_tokens", 0)
                                     + counts.get("cache_creation_input_tokens", 0))
                            cached = counts.get("cache_read_input_tokens", 0)
                            if fresh or cached:
                                samples.append((when, fresh, cached))
                        offset = fh.tell()
                except OSError:
                    continue
            # Keep only what either window could still need.
            horizon = min(cutoff, day_start)
            samples = [s for s in samples if s[0] >= horizon]
            _claude_offsets[key] = (offset, samples)

    window_tokens = window_turns = window_cached = today_tokens = 0
    for offset, samples in _claude_offsets.values():
        for when, fresh, cached in samples:
            if when >= cutoff:
                window_tokens += fresh
                window_cached += cached
                window_turns += 1
            if when >= day_start:
                today_tokens += fresh

    result = {
        "window_hours": CLAUDE_WINDOW_HOURS,
        "window_tokens": window_tokens,
        "window_cached": window_cached,
        "window_turns": window_turns,
        "today_tokens": today_tokens,
        "source": "local transcripts",
    }
    with _claude_lock:
        _claude_cache = (now, result)
    return result


STOCK_CACHE_SECONDS = 25
_stock_lock = threading.Lock()
_stock_cache: dict[str, tuple[float, str]] = {}


def _fetch_chart(code: str) -> str:
    """Return the watch's chart payload for one symbol.

    Tencent's chart endpoints now redirect to HTTPS, which the watch cannot
    speak - it has no TLS and not enough internal RAM to add one. So the
    fetch happens here and the result is flattened into fixed plain-text
    lines that the firmware can read with strtof and no JSON parser:

        T <p1> <p2> ...        intraday prices, oldest first
        K <o> <c> <h> <l>      one line per day, oldest first
    """
    import urllib.request

    lines: list[str] = []

    def get(url: str) -> str:
        request = urllib.request.Request(url, headers={"User-Agent": "watch-bridge/1"})
        with urllib.request.urlopen(request, timeout=10) as response:
            return response.read().decode("utf-8", "replace")

    try:
        raw = get(f"https://web.ifzq.gtimg.cn/appstock/app/minute/query?code={code}")
        points = json.loads(raw)["data"][code]["data"]["data"]
        prices = [entry.split()[1] for entry in points if len(entry.split()) > 1]
        # The watch draws 60 columns; send an evenly spaced subset.
        if len(prices) > 60:
            step = (len(prices) - 1) / 59
            prices = [prices[round(i * step)] for i in range(60)]
        if prices:
            lines.append("T " + " ".join(prices))
    except Exception as exc:  # noqa: BLE001 - a missing chart must not 500
        print(f"  stock {code}: intraday unavailable ({exc})", flush=True)

    try:
        raw = get(
            "https://web.ifzq.gtimg.cn/appstock/app/fqkline/get"
            f"?param={code},day,,,20,qfq"
        )
        data = json.loads(raw)["data"][code]
        candles = data.get("qfqday") or data.get("day") or []
        for candle in candles[-20:]:
            # date, open, close, high, low, volume
            lines.append(f"K {candle[1]} {candle[2]} {candle[3]} {candle[4]}")
    except Exception as exc:  # noqa: BLE001
        print(f"  stock {code}: daily bars unavailable ({exc})", flush=True)

    return "\n".join(lines) + "\n"


def stock_chart(code: str) -> str:
    """Cached wrapper: several watches (or a fast poll) share one upstream hit."""
    now = time.monotonic()
    with _stock_lock:
        hit = _stock_cache.get(code)
        if hit and now - hit[0] < STOCK_CACHE_SECONDS:
            return hit[1]

    payload = _fetch_chart(code)
    with _stock_lock:
        _stock_cache[code] = (now, payload)
    return payload


class Handler(BaseHTTPRequestHandler):
    def _send_text(self, text: str, code: int = 200) -> None:
        data = text.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _send(self, payload: dict[str, Any], code: int = 200) -> None:
        data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self) -> None:
        path = self.path.split("?", 1)[0]
        if path in ("/", "/status"):
            self._send(build_status())
            return
        # Hooks that cannot POST can drive the bridge with a plain GET:
        #   /event?agent=claude&state=working&project=x&detail=y
        if path == "/event":
            fields = _parse_query(self.path)
            record_event(
                fields.get("agent", "agent"),
                fields.get("state", "working"),
                fields.get("project", ""),
                fields.get("detail", ""),
                fields.get("note", ""),
            )
            self._send({"ok": True})
            return
        if path == "/weather":
            self._send(fetch_weather(_parse_query(self.path).get("city", "青岛")))
            return
        # /quote?codes=sz002241,hk09903 - live prices for the desktop widget
        if path == "/quote":
            codes = [c.strip() for c in
                     _parse_query(self.path).get("codes", "").split(",") if c.strip()]
            codes = [c for c in codes if c.replace(".", "").isalnum()][:6]
            self._send({"quotes": [fetch_quote(code) for code in codes]})
            return
        if path == "/claude":
            self._send(claude_usage())
            return
        # /stock?code=sz002241 - chart data the watch cannot fetch itself
        if path == "/stock":
            code = _parse_query(self.path).get("code", "").strip()
            if not code or not code.replace(".", "").isalnum():
                self._send_text("", 400)
                return
            self._send_text(stock_chart(code))
            return
        self.send_error(404, "Not found")

    def do_POST(self) -> None:
        if self.path.split("?", 1)[0] != "/event":
            self.send_error(404, "Not found")
            return

        length = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(length).decode("utf-8", "replace") if length else "{}"
        try:
            body = json.loads(raw) if raw.strip() else {}
        except json.JSONDecodeError:
            body = {}

        # Query params win over the body: Codex hooks name the event on the
        # command line rather than in stdin, so the URL carries agent/state
        # while the piped JSON still supplies cwd and tool_name.
        query = _parse_query(self.path)

        agent = query.get("agent") or body.get("agent") or body.get("source") or "claude"
        state = (
            query.get("state")
            or body.get("state")
            or _state_from_hook(query.get("event") or body.get("hook_event_name", ""))
        )
        project = (
            query.get("project")
            or body.get("project")
            or os.path.basename(str(body.get("cwd", "")).rstrip("/\\"))
        )
        detail = query.get("detail") or body.get("detail") or body.get("tool_name") or ""

        note = query.get("note") or body.get("note") or ""
        record_event(agent, state, project, detail, note)
        self._send({"ok": True, "status": build_status()})

    def log_message(self, fmt: str, *args: object) -> None:
        return  # the watch polls constantly; keep the console readable


def _parse_query(path: str) -> dict[str, str]:
    from urllib.parse import parse_qsl

    query = path.split("?", 1)[1] if "?" in path else ""
    return {key: value for key, value in parse_qsl(query, keep_blank_values=False)}


def _state_from_hook(event: str) -> str:
    return {
        "SessionStart": "idle",
        "UserPromptSubmit": "working",
        "PreToolUse": "working",
        "PostToolUse": "working",
        "SubagentStop": "working",
        # Anything that blocks on a human turns the ring amber.
        "Notification": "waiting",
        "PermissionRequest": "waiting",
        "PermissionDenied": "waiting",
        "Elicitation": "waiting",
        "Stop": "done",
        "StopFailure": "error",
        "PostToolUseFailure": "error",
        "SessionEnd": "idle",
    }.get(event, "working")


HOOK_HELP = """\
Claude Code — add to ~/.claude/settings.json (merge with existing hooks):

{
  "hooks": {
    "UserPromptSubmit": [{"hooks": [{"type": "command", "command": "curl -s -m 2 -X POST http://127.0.0.1:8766/event -H \\"Content-Type: application/json\\" -d @-"}]}],
    "PreToolUse":       [{"hooks": [{"type": "command", "command": "curl -s -m 2 -X POST http://127.0.0.1:8766/event -H \\"Content-Type: application/json\\" -d @-"}]}],
    "Notification":     [{"hooks": [{"type": "command", "command": "curl -s -m 2 -X POST http://127.0.0.1:8766/event -H \\"Content-Type: application/json\\" -d @-"}]}],
    "Stop":             [{"hooks": [{"type": "command", "command": "curl -s -m 2 -X POST http://127.0.0.1:8766/event -H \\"Content-Type: application/json\\" -d @-"}]}]
  }
}

Claude Code pipes the hook JSON (hook_event_name, cwd, tool_name) on stdin,
which "-d @-" forwards; the bridge maps the event name to a colour.

Codex / anything else — fire a GET when a turn starts and ends:

  curl -s "http://127.0.0.1:8766/event?agent=codex&state=working&project=myrepo"
  curl -s "http://127.0.0.1:8766/event?agent=codex&state=done"

States: idle | working | waiting | error | done
"""


TASK_NAME = "AgentStatusBridge"
LOG_MAX_BYTES = 2 * 1024 * 1024


def _redirect_output(path: Path) -> None:
    """Send prints to a file so the hidden autostart instance stays debuggable."""
    try:
        if path.exists() and path.stat().st_size > LOG_MAX_BYTES:
            path.replace(path.with_suffix(".log.old"))
        stream = path.open("a", encoding="utf-8", buffering=1)
    except OSError:
        return
    sys.stdout = stream
    sys.stderr = stream
    print(f"\n=== bridge started {time.strftime('%Y-%m-%d %H:%M:%S')} ===", flush=True)


def _task_command() -> tuple[str, Path]:
    """Build the autostart command: pythonw keeps it windowless."""
    interpreter = Path(sys.executable)
    windowless = interpreter.with_name("pythonw.exe")
    if windowless.exists():
        interpreter = windowless
    script = Path(__file__).resolve()
    log = script.with_name("agent_status.log")
    return f'"{interpreter}" "{script}" --log "{log}"', log


def _startup_script() -> Path:
    startup = (
        Path(os.environ["APPDATA"])
        / "Microsoft"
        / "Windows"
        / "Start Menu"
        / "Programs"
        / "Startup"
    )
    return startup / f"{TASK_NAME}.vbs"


def _install_via_startup_folder(command: str, log: Path) -> int:
    """Fallback when the task scheduler needs rights we do not have.

    A .vbs launcher rather than a .cmd: WScript.Run with a hidden window
    starts pythonw with no console flashing on screen at logon.
    """
    target = _startup_script()
    escaped = command.replace('"', '""')
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(
        'Set sh = CreateObject("WScript.Shell")\r\n'
        f'sh.Run "{escaped}", 0, False\r\n',
        encoding="utf-8",
    )
    print(f"Installed a startup entry at:\n  {target}")
    print(f"  runs: {command}")
    print(f"  log:  {log}")
    subprocess.Popen(["wscript.exe", str(target)], close_fds=True)
    print("Started now; it will start again at every logon.")
    print(f"Remove it with: python {Path(__file__).name} --uninstall")
    return 0


def install_autostart() -> int:
    command, log = _task_command()

    # A scheduled task is the better home (it can restart on failure), but
    # creating one needs rights a normal account may not have.
    create = subprocess.run(
        ["schtasks", "/Create", "/TN", TASK_NAME, "/TR", command, "/SC", "ONLOGON", "/F"],
        capture_output=True,
        text=True,
    )
    if create.returncode != 0:
        print("Task scheduler refused (needs admin); using the startup folder instead.")
        return _install_via_startup_folder(command, log)

    print(f"Installed '{TASK_NAME}' - it now starts at every logon.")
    print(f"  runs: {command}")
    print(f"  log:  {log}")
    run = subprocess.run(
        ["schtasks", "/Run", "/TN", TASK_NAME], capture_output=True, text=True
    )
    print("Started now." if run.returncode == 0 else "Will start at your next logon.")
    print(f"Remove it with: python {Path(__file__).name} --uninstall")
    return 0


def uninstall_autostart() -> int:
    removed = []

    result = subprocess.run(
        ["schtasks", "/Delete", "/TN", TASK_NAME, "/F"], capture_output=True, text=True
    )
    if result.returncode == 0:
        removed.append("scheduled task")

    target = _startup_script()
    if target.exists():
        target.unlink()
        removed.append(f"startup entry ({target.name})")

    if not removed:
        print("Nothing to remove.")
        return 1
    print("Removed: " + ", ".join(removed))
    print("A bridge that is already running keeps going until you end it.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=PORT)
    parser.add_argument("--print-hooks", action="store_true", help="show hook setup and exit")
    parser.add_argument("--log", metavar="FILE", help="append output to FILE instead of stdout")
    parser.add_argument("--install", action="store_true", help="start automatically at logon")
    parser.add_argument("--uninstall", action="store_true", help="remove the logon task")
    args = parser.parse_args()

    if args.print_hooks:
        print(HOOK_HELP)
        return 0
    if args.install:
        return install_autostart()
    if args.uninstall:
        return uninstall_autostart()

    if args.log:
        _redirect_output(Path(args.log))

    try:
        server = BridgeServer((HOST, args.port), Handler)
    except OSError as exc:
        # Nearly always a second copy already listening; say so plainly
        # instead of dumping a traceback into the log every logon.
        print(f"Cannot listen on port {args.port}: {exc}")
        print("Another bridge instance is probably already running.")
        return 1

    print(f"Agent status bridge on http://{HOST}:{args.port}/status")
    print(f"Point AGENT_STATUS_URL in main/app_config.h at this host:{args.port}.")
    print("Run with --print-hooks for hook setup, --install to start at logon.")
    server.serve_forever()
    return 0


if __name__ == "__main__":
    sys.exit(main())
