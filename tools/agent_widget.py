"""Desktop widget for the Codex / Claude Code bridge.

A small always-on-top panel that shows what the watch shows: which agent is
working, what it is working on, how long it has been at it, and how much Codex
quota is left. Same colours as the device, so a glance at either tells you the
same thing.

    python tools/agent_widget.py
    python tools/agent_widget.py --host 192.168.50.37   # bridge elsewhere
    python tools/agent_widget.py --install-autostart

Drag it anywhere; the position is remembered. Right-click for opacity,
always-on-top and quit. Standard library only - no pip install.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import threading
import tkinter as tk
import urllib.error
import urllib.request
from pathlib import Path
from tkinter import font as tkfont

STATUS_PORT = 8766
USAGE_PORT = 8765
STATUS_POLL_MS = 2000
USAGE_POLL_MS = 30000

TASK_NAME = "CodexAgentWidget"
CONFIG_PATH = Path(os.environ.get("APPDATA", ".")) / "codex_agent_widget.json"

# The device's palette, so the widget and the watch never disagree.
BG = "#0B1017"
CARD = "#141C26"
TEXT = "#F7FBFF"
DIM = "#8DA0B4"
FAINT = "#5A6673"
STATE_COLOURS = {
    "idle": "#5A6673",
    "working": "#30D158",
    "waiting": "#FFD60A",
    "error": "#FF453A",
    "done": "#18D7F5",
    "offline": "#3A4450",
}
STATE_LABELS = {
    "idle": "IDLE",
    "working": "WORKING",
    "waiting": "WAITING",
    "error": "ERROR",
    "done": "DONE",
    "offline": "OFFLINE",
}


def fetch_json(url: str, timeout: float = 2.5) -> dict | None:
    try:
        with urllib.request.urlopen(url, timeout=timeout) as response:
            return json.loads(response.read().decode("utf-8"))
    except (urllib.error.URLError, OSError, ValueError, TimeoutError):
        return None


def format_elapsed(seconds: int) -> str:
    if seconds >= 3600:
        return f"{seconds // 3600}h{(seconds % 3600) // 60:02d}m"
    if seconds >= 60:
        return f"{seconds // 60}m{seconds % 60:02d}s"
    return f"{seconds}s"


class Widget:
    def __init__(self, host: str):
        self.host = host
        self.status_url = f"http://{host}:{STATUS_PORT}/status"
        self.usage_url = f"http://{host}:{USAGE_PORT}/usage"
        self.config = self._load_config()

        self.root = tk.Tk()
        self.root.title("Codex / Claude")
        self.root.overrideredirect(True)   # frameless
        self.root.configure(bg=BG)
        self.root.attributes("-topmost", self.config.get("topmost", True))
        self.root.attributes("-alpha", self.config.get("alpha", 0.94))
        geometry = self.config.get("geometry")
        self.root.geometry(geometry if geometry else "+80+80")

        self._build()
        self._bind_drag()
        self._bind_menu()

        self.usage_percent: int | None = None
        self.poll_status()
        self.poll_usage()

    # ------------------------------------------------------------ appearance
    def _build(self) -> None:
        pad = tk.Frame(self.root, bg=BG, padx=14, pady=12)
        pad.pack(fill="both", expand=True)

        title_font = tkfont.Font(family="Segoe UI Semibold", size=10)
        state_font = tkfont.Font(family="Segoe UI Semibold", size=17)
        small_font = tkfont.Font(family="Segoe UI", size=9)
        pill_font = tkfont.Font(family="Segoe UI Semibold", size=8)

        header = tk.Frame(pad, bg=BG)
        header.pack(fill="x")
        self.lamp = tk.Canvas(header, width=14, height=14, bg=BG,
                              highlightthickness=0)
        self.lamp_dot = self.lamp.create_oval(1, 1, 13, 13,
                                              fill=STATE_COLOURS["offline"], width=0)
        self.lamp.pack(side="left", padx=(0, 8))
        self.title = tk.Label(header, text="AI STATUS", font=title_font,
                              bg=BG, fg=DIM)
        self.title.pack(side="left")
        self.updated = tk.Label(header, text="", font=small_font, bg=BG, fg=FAINT)
        self.updated.pack(side="right")

        self.state = tk.Label(pad, text="--", font=state_font, bg=BG,
                              fg=STATE_COLOURS["offline"])
        self.state.pack(anchor="w", pady=(6, 0))

        # Fixed character width: without it a long project name stretches the
        # whole widget and it never settles at one size.
        self.target = tk.Label(pad, text="bridge offline", font=small_font,
                               bg=BG, fg=DIM, anchor="w", justify="left",
                               width=34)
        self.target.pack(anchor="w", fill="x")

        # Per-agent pills.
        pills = tk.Frame(pad, bg=BG)
        pills.pack(fill="x", pady=(10, 0))
        self.pills: dict[str, tk.Label] = {}
        for name in ("CODEX", "CLAUDE"):
            holder = tk.Frame(pills, bg=CARD, padx=8, pady=4)
            holder.pack(side="left", padx=(0, 8))
            tk.Label(holder, text=name, font=pill_font, bg=CARD, fg=FAINT).pack(side="left")
            value = tk.Label(holder, text="--", font=pill_font, bg=CARD, fg=DIM)
            value.pack(side="left", padx=(6, 0))
            self.pills[name.lower()] = value

        # Quota bar.
        quota = tk.Frame(pad, bg=BG)
        quota.pack(fill="x", pady=(12, 0))
        self.quota_label = tk.Label(quota, text="QUOTA --", font=pill_font,
                                    bg=BG, fg=FAINT)
        self.quota_label.pack(anchor="w")
        self.bar = tk.Canvas(quota, height=6, bg=CARD, highlightthickness=0)
        self.bar.pack(fill="x", pady=(4, 0))
        self.bar_fill = self.bar.create_rectangle(0, 0, 0, 6,
                                                  fill=STATE_COLOURS["idle"], width=0)
        self.bar.bind("<Configure>", lambda _event: self._draw_bar())

        self.root.update_idletasks()
        self.root.minsize(232, self.root.winfo_reqheight())

    def _draw_bar(self) -> None:
        width = self.bar.winfo_width()
        if self.usage_percent is None:
            self.bar.coords(self.bar_fill, 0, 0, 0, 6)
            return
        left = max(0, min(100, self.usage_percent))
        colour = ("#FF453A" if left < 15 else
                  "#FFD60A" if left < 35 else "#30D158")
        self.bar.itemconfigure(self.bar_fill, fill=colour)
        self.bar.coords(self.bar_fill, 0, 0, width * left / 100.0, 6)

    # ---------------------------------------------------------------- moving
    def _bind_drag(self) -> None:
        def press(event: tk.Event) -> None:
            self._drag = (event.x_root - self.root.winfo_x(),
                          event.y_root - self.root.winfo_y())

        def drag(event: tk.Event) -> None:
            self.root.geometry(f"+{event.x_root - self._drag[0]}"
                               f"+{event.y_root - self._drag[1]}")

        def release(_event: tk.Event) -> None:
            self._save_config()

        for widget in self._all_widgets(self.root):
            widget.bind("<Button-1>", press)
            widget.bind("<B1-Motion>", drag)
            widget.bind("<ButtonRelease-1>", release)

    def _all_widgets(self, node: tk.Misc) -> list[tk.Misc]:
        found = [node]
        for child in node.winfo_children():
            found.extend(self._all_widgets(child))
        return found

    def _bind_menu(self) -> None:
        menu = tk.Menu(self.root, tearoff=0, bg=CARD, fg=TEXT,
                       activebackground="#1F2A36", activeforeground=TEXT, bd=0)
        self.topmost_var = tk.BooleanVar(value=bool(self.config.get("topmost", True)))
        menu.add_checkbutton(label="Always on top", variable=self.topmost_var,
                             command=self._toggle_topmost)
        opacity = tk.Menu(menu, tearoff=0, bg=CARD, fg=TEXT)
        for value in (1.0, 0.94, 0.8, 0.6):
            opacity.add_command(label=f"{int(value * 100)}%",
                                command=lambda v=value: self._set_alpha(v))
        menu.add_cascade(label="Opacity", menu=opacity)
        menu.add_separator()
        menu.add_command(label=f"Bridge: {self.host}", state="disabled")
        menu.add_command(label="Quit", command=self._quit)

        def popup(event: tk.Event) -> None:
            menu.tk_popup(event.x_root, event.y_root)

        for widget in self._all_widgets(self.root):
            widget.bind("<Button-3>", popup)

    def _toggle_topmost(self) -> None:
        self.root.attributes("-topmost", self.topmost_var.get())
        self._save_config()

    def _set_alpha(self, value: float) -> None:
        self.root.attributes("-alpha", value)
        self._save_config()

    def _quit(self) -> None:
        self._save_config()
        self.root.destroy()

    # ---------------------------------------------------------------- config
    def _load_config(self) -> dict:
        try:
            return json.loads(CONFIG_PATH.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return {}

    def _save_config(self) -> None:
        data = {
            "geometry": f"+{self.root.winfo_x()}+{self.root.winfo_y()}",
            "topmost": bool(self.root.attributes("-topmost")),
            "alpha": float(self.root.attributes("-alpha")),
        }
        try:
            CONFIG_PATH.write_text(json.dumps(data), encoding="utf-8")
        except OSError:
            pass

    # ----------------------------------------------------------------- polls
    def poll_status(self) -> None:
        threading.Thread(target=self._status_worker, daemon=True).start()
        self.root.after(STATUS_POLL_MS, self.poll_status)

    def _status_worker(self) -> None:
        payload = fetch_json(self.status_url)
        self.root.after(0, lambda: self._render_status(payload))

    def _render_status(self, payload: dict | None) -> None:
        if payload is None:
            self.state.configure(text="OFFLINE", fg=STATE_COLOURS["offline"])
            self.lamp.itemconfigure(self.lamp_dot, fill=STATE_COLOURS["offline"])
            self.target.configure(text=f"no bridge at {self.host}")
            self.updated.configure(text="")
            for value in self.pills.values():
                value.configure(text="--", fg=DIM)
            return

        state = payload.get("state", "idle")
        colour = STATE_COLOURS.get(state, STATE_COLOURS["idle"])
        self.state.configure(text=STATE_LABELS.get(state, state.upper()), fg=colour)
        self.lamp.itemconfigure(self.lamp_dot, fill=colour)

        agent = payload.get("agent", "")
        project = payload.get("project", "")
        detail = payload.get("detail", "")
        elapsed = format_elapsed(int(payload.get("elapsed", 0) or 0))
        line = " · ".join(part for part in (agent, project) if part)
        if detail:
            line = f"{line} — {detail}" if line else detail
        if line:
            room = 34 - len(elapsed) - 3
            if len(line) > room:
                line = line[:room - 1] + "…"
            line = f"{line}   {elapsed}"
        else:
            line = elapsed
        self.target.configure(text=line)
        self.updated.configure(text=payload.get("updated", ""))

        for name in ("codex", "claude"):
            value = payload.get(name, "") or "idle"
            self.pills[name].configure(
                text=STATE_LABELS.get(value, value).lower(),
                fg=STATE_COLOURS.get(value, DIM))

    def poll_usage(self) -> None:
        threading.Thread(target=self._usage_worker, daemon=True).start()
        self.root.after(USAGE_POLL_MS, self.poll_usage)

    def _usage_worker(self) -> None:
        payload = fetch_json(self.usage_url)
        self.root.after(0, lambda: self._render_usage(payload))

    def _render_usage(self, payload: dict | None) -> None:
        if payload is None:
            self.usage_percent = None
            self.quota_label.configure(text="QUOTA --", fg=FAINT)
            self._draw_bar()
            return

        used = payload.get("used_percent")
        if used is None:
            limit = float(payload.get("limit_tokens") or 0)
            used = (float(payload.get("used_tokens") or 0) / limit * 100.0) if limit else 0
        left = max(0, min(100, int(round(100 - float(used)))))
        self.usage_percent = left
        reset = payload.get("reset_at_local", "")
        text = f"QUOTA {left}% LEFT"
        if reset:
            text += f"   resets {reset}"
        self.quota_label.configure(text=text, fg=DIM)
        self._draw_bar()

    def run(self) -> None:
        self.root.mainloop()


# ------------------------------------------------------------------ autostart
def _startup_script() -> Path:
    startup = (Path(os.environ["APPDATA"]) / "Microsoft" / "Windows"
               / "Start Menu" / "Programs" / "Startup")
    return startup / f"{TASK_NAME}.vbs"


def install_autostart(host: str) -> int:
    """A .vbs launcher, same trick the bridge uses: WScript.Run with a hidden
    window starts pythonw at logon with no console flashing up."""
    pythonw = Path(sys.executable).with_name("pythonw.exe")
    runner = pythonw if pythonw.exists() else Path(sys.executable)
    command = f'"{runner}" "{Path(__file__).resolve()}" --host {host}'

    target = _startup_script()
    target.parent.mkdir(parents=True, exist_ok=True)
    escaped = command.replace('"', '""')
    target.write_text('Set sh = CreateObject("WScript.Shell")\r\n'
                      f'sh.Run "{escaped}", 0, False\r\n', encoding="utf-8")
    print(f"Installed a startup entry at:\n  {target}\n  runs: {command}")
    subprocess.Popen(["wscript.exe", str(target)], close_fds=True)
    print("Started now; it will start again at every logon.")
    print(f"Remove it with: python {Path(__file__).name} --uninstall")
    return 0


def uninstall_autostart() -> int:
    target = _startup_script()
    if target.exists():
        target.unlink()
        print(f"Removed {target}")
    else:
        print("Nothing installed.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1",
                        help="where the bridge runs (default: this machine)")
    parser.add_argument("--install-autostart", action="store_true")
    parser.add_argument("--uninstall", action="store_true")
    args = parser.parse_args()

    if args.install_autostart:
        return install_autostart(args.host)
    if args.uninstall:
        return uninstall_autostart()

    Widget(args.host).run()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
