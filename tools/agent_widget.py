"""Desktop widget for the Codex / Claude Code bridge.

Three tabs behind a compact always-on-top panel, in the device's own colours:

    AI       agent state, Codex quota, Claude's token spend
    WEATHER  today plus the next three days
    STOCK    price, intraday trace and 20-day candles, for your own symbols

    python tools/agent_widget.py
    python tools/agent_widget.py --host 192.168.50.37   # bridge elsewhere
    python tools/agent_widget.py --city 上海 --codes sz002241,sh600519
    python tools/agent_widget.py --install-autostart

Only the visible tab is polled, so the panel costs one request every few
seconds no matter how much it can show. Drag it anywhere; position, tab and
symbol list are remembered. Right-click for opacity, always-on-top and quit.
Standard library only - no pip install.
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
import urllib.parse
import urllib.request
from pathlib import Path
from tkinter import font as tkfont

STATUS_PORT = 8766
USAGE_PORT = 8765
STATUS_POLL_MS = 2000       # the lamp, whatever tab is open
TAB_POLL_MS = 60000         # weather and quotes
USAGE_POLL_MS = 30000       # Codex quota and Claude tokens

DEFAULT_CITY = "青岛"
DEFAULT_CODES = "sz002241,hk09903"

TASK_NAME = "CodexAgentWidget"
CONFIG_PATH = Path(os.environ.get("APPDATA", ".")) / "codex_agent_widget.json"

# The device's palette, so the widget and the watch never disagree.
BG = "#0B1017"
CARD = "#141C26"
LINE = "#1B2430"
TEXT = "#F7FBFF"
DIM = "#8DA0B4"
FAINT = "#5A6673"
ACCENT = "#18D7F5"
UP = "#FF453A"      # red for up: this market's convention, and the watch's
DOWN = "#30D158"

STATE_COLOURS = {
    "idle": "#5A6673", "working": "#30D158", "waiting": "#FFD60A",
    "error": "#FF453A", "done": "#18D7F5", "offline": "#3A4450",
}
STATE_LABELS = {
    "idle": "IDLE", "working": "WORKING", "waiting": "WAITING",
    "error": "ERROR", "done": "DONE", "offline": "OFFLINE",
}


def fetch_json(url: str, timeout: float = 3.0) -> dict | None:
    try:
        with urllib.request.urlopen(url, timeout=timeout) as response:
            return json.loads(response.read().decode("utf-8"))
    except (urllib.error.URLError, OSError, ValueError, TimeoutError):
        return None


def fetch_text(url: str, timeout: float = 12.0) -> str | None:
    try:
        with urllib.request.urlopen(url, timeout=timeout) as response:
            return response.read().decode("utf-8", errors="replace")
    except (urllib.error.URLError, OSError, TimeoutError):
        return None


def format_tokens(count: int) -> str:
    if count >= 1_000_000:
        return f"{count / 1_000_000:.1f}M"
    if count >= 1_000:
        return f"{count / 1_000:.0f}K"
    return str(count)


def format_elapsed(seconds: int) -> str:
    if seconds >= 3600:
        return f"{seconds // 3600}h{(seconds % 3600) // 60:02d}m"
    if seconds >= 60:
        return f"{seconds // 60}m{seconds % 60:02d}s"
    return f"{seconds}s"


def parse_chart(text: str) -> tuple[list[float], list[tuple[float, float, float, float]]]:
    """The bridge's flat chart format: 'T p p p ...' then one 'K o c h l' a day."""
    trend: list[float] = []
    candles: list[tuple[float, float, float, float]] = []
    for line in text.splitlines():
        parts = line.split()
        if not parts:
            continue
        if parts[0] == "T":
            trend = [float(value) for value in parts[1:]]
        elif parts[0] == "K" and len(parts) >= 5:
            candles.append((float(parts[1]), float(parts[2]),
                            float(parts[3]), float(parts[4])))
    return trend, candles


class Widget:
    def __init__(self, host: str, city: str, codes: str):
        self.host = host
        self.config = self._load_config()
        self.city = self.config.get("city", city)
        self.codes = [c.strip() for c in
                      self.config.get("codes", codes).split(",") if c.strip()]
        self.symbol = 0
        self.chart_mode = self.config.get("chart_mode", "intraday")
        self.tab = self.config.get("tab", "ai")

        self.status_url = f"http://{host}:{STATUS_PORT}/status"
        self.usage_url = f"http://{host}:{USAGE_PORT}/usage"
        self.claude_url = f"http://{host}:{STATUS_PORT}/claude"

        self.root = tk.Tk()
        self.root.title("Codex / Claude")
        self.root.overrideredirect(True)
        self.root.configure(bg=BG)
        self.root.attributes("-topmost", self.config.get("topmost", True))
        self.root.attributes("-alpha", self.config.get("alpha", 0.94))
        self.root.geometry(self.config.get("geometry", "+80+80"))

        self.fonts = {
            "title": tkfont.Font(family="Segoe UI Semibold", size=10),
            "state": tkfont.Font(family="Segoe UI Semibold", size=17),
            "big": tkfont.Font(family="Segoe UI Semibold", size=22),
            "small": tkfont.Font(family="Segoe UI", size=9),
            "pill": tkfont.Font(family="Segoe UI Semibold", size=8),
        }

        self.trend: list[float] = []
        self.candles: list[tuple[float, float, float, float]] = []
        self.usage_percent: int | None = None

        self._build()
        self._bind_drag()
        self._bind_menu()
        self._show_tab(self.tab)

        self.poll_status()
        self.poll_usage()
        self.poll_tab()

    # ---------------------------------------------------------------- layout
    def _build(self) -> None:
        self.outer = tk.Frame(self.root, bg=BG, padx=14, pady=12)
        self.outer.pack(fill="both", expand=True)

        # The header is shared: the lamp is worth seeing from every tab.
        header = tk.Frame(self.outer, bg=BG)
        header.pack(fill="x")
        self.lamp = tk.Canvas(header, width=14, height=14, bg=BG, highlightthickness=0)
        self.lamp_dot = self.lamp.create_oval(1, 1, 13, 13,
                                              fill=STATE_COLOURS["offline"], width=0)
        self.lamp.pack(side="left", padx=(0, 8))
        self.header_title = tk.Label(header, text="AI STATUS", font=self.fonts["title"],
                                     bg=BG, fg=DIM)
        self.header_title.pack(side="left")
        self.updated = tk.Label(header, text="", font=self.fonts["small"],
                                bg=BG, fg=FAINT)
        self.updated.pack(side="right")

        # Fixed body: the panel must not resize as tabs change.
        self.body = tk.Frame(self.outer, bg=BG, width=360, height=168)
        self.body.pack(fill="both", expand=True, pady=(8, 0))
        self.body.pack_propagate(False)

        self.pages = {
            "ai": self._build_ai(),
            "weather": self._build_weather(),
            "stock": self._build_stock(),
        }

        tabs = tk.Frame(self.outer, bg=BG)
        tabs.pack(fill="x", pady=(10, 0))
        self.tab_buttons: dict[str, tk.Label] = {}
        for key, label in (("ai", "AI"), ("weather", "天气"), ("stock", "股票")):
            button = tk.Label(tabs, text=label, font=self.fonts["pill"], bg=CARD,
                              fg=DIM, padx=12, pady=5)
            button.pack(side="left", padx=(0, 6))
            button.bind("<Button-1>", lambda _event, k=key: self._show_tab(k))
            self.tab_buttons[key] = button

    def _build_ai(self) -> tk.Frame:
        page = tk.Frame(self.body, bg=BG)
        self.state = tk.Label(page, text="--", font=self.fonts["state"], bg=BG,
                              fg=STATE_COLOURS["offline"])
        self.state.pack(anchor="w")
        self.target = tk.Label(page, text="bridge offline", font=self.fonts["small"],
                               bg=BG, fg=DIM, anchor="w", width=34)
        self.target.pack(anchor="w", fill="x")

        pills = tk.Frame(page, bg=BG)
        pills.pack(fill="x", pady=(10, 0))
        self.pills: dict[str, tk.Label] = {}
        for name in ("CODEX", "CLAUDE"):
            holder = tk.Frame(pills, bg=CARD, padx=8, pady=4)
            holder.pack(side="left", padx=(0, 8))
            tk.Label(holder, text=name, font=self.fonts["pill"],
                     bg=CARD, fg=FAINT).pack(side="left")
            value = tk.Label(holder, text="--", font=self.fonts["pill"], bg=CARD, fg=DIM)
            value.pack(side="left", padx=(6, 0))
            self.pills[name.lower()] = value

        self.quota_label = tk.Label(page, text="CODEX --", font=self.fonts["pill"],
                                    bg=BG, fg=FAINT, anchor="w")
        self.quota_label.pack(anchor="w", fill="x", pady=(12, 0))
        self.bar = tk.Canvas(page, height=6, bg=CARD, highlightthickness=0)
        self.bar.pack(fill="x", pady=(4, 0))
        self.bar_fill = self.bar.create_rectangle(0, 0, 0, 6, fill=DOWN, width=0)
        self.bar.bind("<Configure>", lambda _event: self._draw_bar())

        self.claude_label = tk.Label(page, text="CLAUDE --", font=self.fonts["pill"],
                                     bg=BG, fg=FAINT, anchor="w")
        self.claude_label.pack(anchor="w", fill="x", pady=(8, 0))
        return page

    def _build_weather(self) -> tk.Frame:
        page = tk.Frame(self.body, bg=BG)
        today = tk.Frame(page, bg=BG)
        today.pack(fill="x")
        self.w_temp = tk.Label(today, text="--°", font=self.fonts["big"], bg=BG, fg=TEXT)
        self.w_temp.pack(side="left")
        block = tk.Frame(today, bg=BG)
        block.pack(side="left", padx=(12, 0))
        self.w_condition = tk.Label(block, text="--", font=self.fonts["small"],
                                    bg=BG, fg="#FFD166", anchor="w")
        self.w_condition.pack(anchor="w")
        self.w_range = tk.Label(block, text="--", font=self.fonts["small"],
                                bg=BG, fg=DIM, anchor="w")
        self.w_range.pack(anchor="w")
        self.w_city = tk.Label(today, text="", font=self.fonts["small"], bg=BG, fg=FAINT)
        self.w_city.pack(side="right")

        tk.Frame(page, bg=LINE, height=1).pack(fill="x", pady=(12, 8))

        self.w_days: list[tuple[tk.Label, tk.Label, tk.Label]] = []
        for _ in range(3):
            row = tk.Frame(page, bg=BG)
            row.pack(fill="x", pady=3)
            date = tk.Label(row, text="", font=self.fonts["small"], bg=BG,
                            fg=DIM, width=6, anchor="w")
            date.pack(side="left")
            condition = tk.Label(row, text="", font=self.fonts["small"], bg=BG,
                                 fg=TEXT, anchor="w")
            condition.pack(side="left")
            span = tk.Label(row, text="", font=self.fonts["small"], bg=BG, fg=DIM)
            span.pack(side="right")
            self.w_days.append((date, condition, span))
        return page

    def _build_stock(self) -> tk.Frame:
        page = tk.Frame(self.body, bg=BG)

        head = tk.Frame(page, bg=BG)
        head.pack(fill="x")
        self.s_name = tk.Label(head, text="--", font=self.fonts["title"], bg=BG, fg=TEXT)
        self.s_name.pack(side="left")
        self.s_price = tk.Label(head, text="--", font=self.fonts["title"], bg=BG, fg=DIM)
        self.s_price.pack(side="right")
        self.s_change = tk.Label(page, text="", font=self.fonts["small"], bg=BG,
                                 fg=DIM, anchor="w")
        self.s_change.pack(anchor="w")

        self.chart = tk.Canvas(page, height=62, bg=CARD, highlightthickness=0)
        self.chart.pack(fill="x", pady=(6, 0))
        self.chart.bind("<Configure>", lambda _event: self._draw_chart())

        controls = tk.Frame(page, bg=BG)
        controls.pack(fill="x", pady=(8, 0))
        self.s_chips = tk.Frame(controls, bg=BG)
        self.s_chips.pack(side="left")
        self.s_mode = tk.Label(controls, text="日内", font=self.fonts["pill"],
                               bg=CARD, fg=ACCENT, padx=8, pady=3)
        self.s_mode.pack(side="right")
        self.s_mode.bind("<Button-1>", lambda _event: self._toggle_chart())

        entry_row = tk.Frame(page, bg=BG)
        entry_row.pack(fill="x", pady=(8, 0))
        self.s_entry = tk.Entry(entry_row, font=self.fonts["small"], bg=CARD, fg=TEXT,
                                insertbackground=TEXT, relief="flat", width=22)
        self.s_entry.pack(side="left", ipady=3)
        self.s_entry.insert(0, ",".join(self.codes))
        apply_button = tk.Label(entry_row, text="应用", font=self.fonts["pill"],
                                bg=CARD, fg=ACCENT, padx=8, pady=3)
        apply_button.pack(side="left", padx=(6, 0))
        apply_button.bind("<Button-1>", lambda _event: self._apply_codes())
        self.s_entry.bind("<Return>", lambda _event: self._apply_codes())

        self._rebuild_chips()
        return page

    def _rebuild_chips(self) -> None:
        for child in self.s_chips.winfo_children():
            child.destroy()
        for index, code in enumerate(self.codes[:4]):
            active = index == self.symbol
            chip = tk.Label(self.s_chips, text=code.upper(), font=self.fonts["pill"],
                            bg=ACCENT if active else CARD,
                            fg="#03181F" if active else DIM, padx=7, pady=3)
            chip.pack(side="left", padx=(0, 5))
            chip.bind("<Button-1>", lambda _event, i=index: self._select_symbol(i))

    # ------------------------------------------------------------------ tabs
    def _show_tab(self, key: str) -> None:
        self.tab = key
        for page in self.pages.values():
            page.pack_forget()
        self.pages[key].pack(fill="both", expand=True)
        for name, button in self.tab_buttons.items():
            active = name == key
            button.configure(bg=ACCENT if active else CARD,
                             fg="#03181F" if active else DIM)
        self.header_title.configure(
            text={"ai": "AI STATUS", "weather": "WEATHER", "stock": "STOCK"}[key])
        self._save_config()
        self.poll_tab(once=True)

    def _select_symbol(self, index: int) -> None:
        self.symbol = index
        self._rebuild_chips()
        self.poll_tab(once=True)

    def _apply_codes(self) -> None:
        codes = [c.strip() for c in self.s_entry.get().split(",") if c.strip()]
        if not codes:
            return
        self.codes = codes[:4]
        self.symbol = 0
        self._rebuild_chips()
        self._save_config()
        self.poll_tab(once=True)

    def _toggle_chart(self) -> None:
        self.chart_mode = "candles" if self.chart_mode == "intraday" else "intraday"
        self.s_mode.configure(text="日K" if self.chart_mode == "candles" else "日内")
        self._save_config()
        self._draw_chart()

    # --------------------------------------------------------------- drawing
    def _draw_bar(self) -> None:
        width = self.bar.winfo_width()
        if self.usage_percent is None:
            self.bar.coords(self.bar_fill, 0, 0, 0, 6)
            return
        left = max(0, min(100, self.usage_percent))
        self.bar.itemconfigure(
            self.bar_fill,
            fill=UP if left < 15 else "#FFD60A" if left < 35 else DOWN)
        self.bar.coords(self.bar_fill, 0, 0, width * left / 100.0, 6)

    def _draw_chart(self) -> None:
        self.chart.delete("plot")
        width = self.chart.winfo_width()
        height = self.chart.winfo_height()
        if width < 20 or height < 20:
            return
        pad = 6

        if self.chart_mode == "intraday":
            series = self.trend
            if len(series) < 2:
                return
            low, high = min(series), max(series)
            span = (high - low) or 1.0
            step = (width - 2 * pad) / (len(series) - 1)
            points: list[float] = []
            for index, value in enumerate(series):
                points.append(pad + index * step)
                points.append(height - pad - (value - low) / span * (height - 2 * pad))
            colour = UP if series[-1] >= series[0] else DOWN
            self.chart.create_line(*points, fill=colour, width=2, tags="plot")
            return

        series = self.candles
        if not series:
            return
        low = min(candle[3] for candle in series)
        high = max(candle[2] for candle in series)
        span = (high - low) or 1.0
        slot = (width - 2 * pad) / len(series)
        body_w = max(2.0, slot * 0.6)

        def y_of(value: float) -> float:
            return height - pad - (value - low) / span * (height - 2 * pad)

        for index, (open_, close, hi, lo) in enumerate(series):
            centre = pad + slot * (index + 0.5)
            colour = UP if close >= open_ else DOWN
            self.chart.create_line(centre, y_of(hi), centre, y_of(lo),
                                   fill=colour, tags="plot")
            top, bottom = y_of(max(open_, close)), y_of(min(open_, close))
            if bottom - top < 1:
                bottom = top + 1
            self.chart.create_rectangle(centre - body_w / 2, top,
                                        centre + body_w / 2, bottom,
                                        fill=colour, outline=colour, tags="plot")

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

        # Controls own their clicks; everything else is a drag handle.
        skip = set(self.tab_buttons.values()) | {self.s_mode, self.s_entry, self.chart}
        for widget in self._all_widgets(self.root):
            if widget in skip or isinstance(widget, tk.Entry):
                continue
            if widget.master is self.s_chips or getattr(widget, "_no_drag", False):
                continue
            widget.bind("<Button-1>", press, add="+")
            widget.bind("<B1-Motion>", drag, add="+")
            widget.bind("<ButtonRelease-1>", release, add="+")

    def _all_widgets(self, node: tk.Misc) -> list[tk.Misc]:
        found: list[tk.Misc] = [node]
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

        for widget in self._all_widgets(self.root):
            widget.bind("<Button-3>", lambda event: menu.tk_popup(event.x_root,
                                                                  event.y_root))

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
        try:
            CONFIG_PATH.write_text(json.dumps({
                "geometry": f"+{self.root.winfo_x()}+{self.root.winfo_y()}",
                "topmost": bool(self.root.attributes("-topmost")),
                "alpha": float(self.root.attributes("-alpha")),
                "tab": self.tab,
                "city": self.city,
                "codes": ",".join(self.codes),
                "chart_mode": self.chart_mode,
            }, ensure_ascii=False), encoding="utf-8")
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
            self.lamp.itemconfigure(self.lamp_dot, fill=STATE_COLOURS["offline"])
            self.state.configure(text="OFFLINE", fg=STATE_COLOURS["offline"])
            self.target.configure(text=f"no bridge at {self.host}")
            self.updated.configure(text="")
            for value in self.pills.values():
                value.configure(text="--", fg=DIM)
            return

        state = payload.get("state", "idle")
        colour = STATE_COLOURS.get(state, STATE_COLOURS["idle"])
        self.lamp.itemconfigure(self.lamp_dot, fill=colour)
        self.state.configure(text=STATE_LABELS.get(state, state.upper()), fg=colour)

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
            self.pills[name].configure(text=STATE_LABELS.get(value, value).lower(),
                                       fg=STATE_COLOURS.get(value, DIM))

    def poll_usage(self) -> None:
        threading.Thread(target=self._usage_worker, daemon=True).start()
        self.root.after(USAGE_POLL_MS, self.poll_usage)

    def _usage_worker(self) -> None:
        usage = fetch_json(self.usage_url)
        claude = fetch_json(self.claude_url, timeout=8.0)
        self.root.after(0, lambda: self._render_usage(usage, claude))

    def _render_usage(self, usage: dict | None, claude: dict | None) -> None:
        if usage is None:
            self.usage_percent = None
            self.quota_label.configure(text="CODEX --", fg=FAINT)
        else:
            used = usage.get("used_percent")
            if used is None:
                limit = float(usage.get("limit_tokens") or 0)
                used = (float(usage.get("used_tokens") or 0) / limit * 100.0) if limit else 0
            self.usage_percent = max(0, min(100, int(round(100 - float(used)))))
            reset = usage.get("reset_at_local", "")
            text = f"CODEX {self.usage_percent}% LEFT"
            if reset:
                text += f"   resets {reset}"
            self.quota_label.configure(text=text, fg=DIM)
        self._draw_bar()

        if claude and "window_tokens" in claude:
            hours = float(claude.get("window_hours", 5))
            self.claude_label.configure(
                text=f"CLAUDE {format_tokens(claude['window_tokens'])} tok · "
                     f"{claude['window_turns']} turns / {hours:.0f}h",
                fg=DIM)
        else:
            self.claude_label.configure(text="CLAUDE --", fg=FAINT)

    def poll_tab(self, once: bool = False) -> None:
        """Only the open tab is fetched: the panel should not cost three
        requests a minute to show one of them."""
        if self.tab == "weather":
            threading.Thread(target=self._weather_worker, daemon=True).start()
        elif self.tab == "stock":
            threading.Thread(target=self._stock_worker, daemon=True).start()
        if not once:
            self.root.after(TAB_POLL_MS, self.poll_tab)

    def _weather_worker(self) -> None:
        url = (f"http://{self.host}:{STATUS_PORT}/weather"
               f"?city={urllib.parse.quote(self.city)}")
        payload = fetch_json(url, timeout=8.0)
        self.root.after(0, lambda: self._render_weather(payload))

    def _render_weather(self, payload: dict | None) -> None:
        if not payload or "temp" not in payload:
            self.w_temp.configure(text="--°")
            self.w_condition.configure(text="weather unavailable")
            self.w_range.configure(text="")
            return
        self.w_temp.configure(text=f"{payload['temp']}°")
        self.w_condition.configure(text=payload["condition"])
        self.w_range.configure(text=f"{payload['low']}° / {payload['high']}°")
        self.w_city.configure(text=f"{payload['city']}  {payload.get('updated', '')}")

        days = payload.get("days", [])[1:4]
        for (date, condition, span), day in zip(self.w_days, days):
            date.configure(text=day["date"])
            condition.configure(text=day["condition"])
            span.configure(text=f"{day['low']}° / {day['high']}°")
        for index in range(len(days), len(self.w_days)):
            for label in self.w_days[index]:
                label.configure(text="")

    def _stock_worker(self) -> None:
        if not self.codes:
            return
        code = self.codes[min(self.symbol, len(self.codes) - 1)]
        quote = fetch_json(f"http://{self.host}:{STATUS_PORT}/quote"
                           f"?codes={urllib.parse.quote(code)}", timeout=8.0)
        chart = fetch_text(f"http://{self.host}:{STATUS_PORT}/stock?code={code}")
        self.root.after(0, lambda: self._render_stock(quote, chart))

    def _render_stock(self, quote: dict | None, chart: str | None) -> None:
        rows = (quote or {}).get("quotes", [])
        if rows and "price" in rows[0]:
            row = rows[0]
            colour = UP if row["change"] >= 0 else DOWN
            self.s_name.configure(text=row["name"], fg=TEXT)
            self.s_price.configure(text=f"{row['price']:.2f}", fg=colour)
            arrow = "▲" if row["change"] >= 0 else "▼"
            self.s_change.configure(
                text=f"{arrow} {row['change']:+.2f}   {row['change_pct']:+.2f}%",
                fg=colour)
        else:
            self.s_name.configure(text="--", fg=DIM)
            self.s_price.configure(text="--", fg=DIM)
            self.s_change.configure(text="quote unavailable", fg=FAINT)

        if chart:
            self.trend, self.candles = parse_chart(chart)
        self._draw_chart()

    def run(self) -> None:
        self.root.mainloop()


# ------------------------------------------------------------------ autostart
def _startup_script() -> Path:
    startup = (Path(os.environ["APPDATA"]) / "Microsoft" / "Windows"
               / "Start Menu" / "Programs" / "Startup")
    return startup / f"{TASK_NAME}.vbs"


def install_autostart(host: str) -> int:
    """A .vbs launcher, the same trick the bridge uses: WScript.Run with a
    hidden window starts pythonw at logon with no console flashing up."""
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
    parser.add_argument("--city", default=DEFAULT_CITY, help="weather city")
    parser.add_argument("--codes", default=DEFAULT_CODES,
                        help="comma-separated stock codes, up to four")
    parser.add_argument("--install-autostart", action="store_true")
    parser.add_argument("--uninstall", action="store_true")
    args = parser.parse_args()

    if args.install_autostart:
        return install_autostart(args.host)
    if args.uninstall:
        return uninstall_autostart()

    Widget(args.host, args.city, args.codes).run()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
