#!/usr/bin/env python3
"""Run the editor's tests in headless Chromium and print their results.

    run_tests.py [chromium binary]

Serves the repository over HTTP on a free localhost port (the tests read the
firmware's golden SysEx vectors from tools/host_render/golden/), loads
tools/editor/test/index.html, and prints what the page reports. Exits 0 if
every check passed.
"""

import functools
import html
import http.server
import re
import subprocess
import sys
import threading
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]


class QuietHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *args):
        pass


def main():
    chromium = sys.argv[1] if len(sys.argv) > 1 else "chromium"
    handler = functools.partial(QuietHandler, directory=str(REPO))
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    url = f"http://127.0.0.1:{server.server_address[1]}/tools/editor/test/index.html"
    try:
        dom = subprocess.run(
            [chromium, "--headless=new", "--disable-gpu", "--no-first-run", "--no-sandbox",
             "--virtual-time-budget=20000", "--dump-dom", url],
            capture_output=True, text=True, timeout=120).stdout
    finally:
        server.shutdown()
    m = re.search(r'<pre id="results"[^>]*>(.*?)</pre>', dom, re.S)
    if not m:
        sys.exit("no test results in the page (did it fail to load?)")
    print(html.unescape(m.group(1)), end="")
    sys.exit(0 if 'data-done="pass"' in dom else 1)


if __name__ == "__main__":
    main()
