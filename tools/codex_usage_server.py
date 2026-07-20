from __future__ import annotations

import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


ROOT = Path(__file__).resolve().parent
USAGE_FILE = ROOT / "codex_usage.json"


class Handler(BaseHTTPRequestHandler):
    def do_GET(self) -> None:
        if self.path not in ("/", "/usage"):
            self.send_error(404, "Not found")
            return

        try:
            payload = json.loads(USAGE_FILE.read_text(encoding="utf-8"))
        except FileNotFoundError:
            payload = {
                "label": "Codex today",
                "used_tokens": 0,
                "limit_tokens": 500000,
                "updated": "missing codex_usage.json",
            }

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
    print(f"Serving {USAGE_FILE} at http://{host}:{port}/usage")
    print("Edit codex_usage.json to change what the board displays.")
    server.serve_forever()


if __name__ == "__main__":
    main()
