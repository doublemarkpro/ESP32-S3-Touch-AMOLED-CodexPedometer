"""Codex hook shim: report state to the agent status bridge with the project name.

Codex names the event on the command line rather than in the payload, and the
project directory only exists in the JSON piped on stdin. curl can carry one or
the other but not both without risking a stall on a missing stdin, so this
shim reads stdin defensively and always exits 0 — a status light must never
block or fail the agent it is reporting on.

Usage (from ~/.codex/hooks.json):
    py -3 <path>/codex_hook.py --state working
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import threading
import urllib.parse
import urllib.request

BRIDGE = "http://127.0.0.1:8766/event"
STDIN_WAIT_SECONDS = 0.4
HTTP_TIMEOUT_SECONDS = 1.5


def read_stdin_payload() -> dict:
    """Read the hook JSON, giving up quickly if nothing is piped in."""
    if sys.stdin is None or sys.stdin.closed:
        return {}
    try:
        if sys.stdin.isatty():  # interactive console: nothing will ever arrive
            return {}
    except Exception:
        return {}

    result: dict = {}

    def worker() -> None:
        try:
            raw = sys.stdin.read()
            if raw and raw.strip():
                parsed = json.loads(raw)
                if isinstance(parsed, dict):
                    result.update(parsed)
        except Exception:
            pass

    thread = threading.Thread(target=worker, daemon=True)
    thread.start()
    thread.join(STDIN_WAIT_SECONDS)
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--state", default="working")
    parser.add_argument("--agent", default="codex")
    args, _ = parser.parse_known_args()

    payload = read_stdin_payload()
    cwd = str(payload.get("cwd") or payload.get("workspace_root") or "")
    project = os.path.basename(cwd.rstrip("/\\")) if cwd else ""
    detail = str(payload.get("tool_name") or "")

    fields = {"agent": args.agent, "state": args.state}
    if project:
        fields["project"] = project
    if detail:
        fields["detail"] = detail

    try:
        urllib.request.urlopen(
            f"{BRIDGE}?{urllib.parse.urlencode(fields)}", timeout=HTTP_TIMEOUT_SECONDS
        ).close()
    except Exception:
        pass  # bridge down is not the agent's problem

    return 0


if __name__ == "__main__":
    sys.exit(main())
