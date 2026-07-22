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
import subprocess
import sys
import threading
import time
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


class Handler(BaseHTTPRequestHandler):
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
