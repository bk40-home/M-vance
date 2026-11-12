#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Sound match in the browser: a small web server on this computer for match.py. Drag a WAV onto the page, press
Run, watch the search, hear and download the result.

  python3 tools/match/serve.py [--port 8765] [--workers N] [--no-open]
  (or double-click tools/match/start-match.bat on Windows: it starts this in WSL)

It opens http://localhost:8765 in the browser (Windows' own browser when run in WSL). Runs go one at a time, in the
order dropped; each is kept in build/match/web/<run id>/ (the sample, the .syx files, match.wav, report.html), and
the page lists them. Only this computer can reach the page (it listens on 127.0.0.1). Stop it with Ctrl+C.

Python standard library only (plus what match.py needs). The page is match.html, next to this file.
"""
import argparse
import json
import queue
import re
import shutil
import subprocess
import sys
import threading
import time
import traceback
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import parse_qs, urlparse

sys.path.insert(0, str(Path(__file__).resolve().parent))
import deps                                            # noqa: E402
deps.require()                                         # (stops here, with the fix, if a package is missing)
import engine_mvance as em                             # noqa: E402

HERE = Path(__file__).resolve().parent
RUNS = em.ROOT / "build/match/web"                     # one folder per run
MAX_UPLOAD = 200 * 1024 * 1024                         # bytes: the largest sample taken
AUDIO_EXT = {".wav", ".aif", ".aiff", ".flac", ".ogg"}  # what soundfile reads
FILES = {"report.html": "text/html; charset=utf-8", "match.wav": "audio/wav", "target.wav": "audio/wav",
         "seed.json": "application/json", "log.txt": "text/plain; charset=utf-8"}
BUDGETS = {"quick": 4000, "normal": 8000, "thorough": 16000}   # renders per search setting
RENDERS_RE = re.compile(r"\s(\d+) renders")            # the render count in a log line (progress)
EDITOR = em.ROOT / "sim/bank"                           # the bank editor, served at /editor/ ("Open in bank editor":
EDITOR_FILES = {"editor.html": "text/html; charset=utf-8",   #  localhost is a secure context, so WebMIDI works)
                "editor.js": "text/javascript; charset=utf-8", "bank-model.js": "text/javascript; charset=utf-8",
                "fm1-midi.js": "text/javascript; charset=utf-8"}

JOBS = {}                                              # run id -> job dict (this session's runs)
WORK = queue.Queue()                                   # run ids waiting
LOCK = threading.Lock()


def run_id(name: str) -> str:
    """A folder-safe id: time, then the sample's name."""
    stem = re.sub(r"[^A-Za-z0-9_-]+", "_", Path(name).stem).strip("_")[:40] or "sample"
    return time.strftime("%Y%m%d-%H%M%S-") + stem


def job_view(j: dict) -> dict:
    """What /status and /runs send about a job."""
    return {k: j.get(k) for k in ("id", "name", "state", "progress", "result", "error", "budget", "note",
                                  "gate_ms", "queued_at")} | {"log": j.get("log", [])[-400:]}


def worker(workers):
    """The one job thread: takes run ids in order and runs match.run_one on each, the engine built once."""
    import match
    eng = None
    while True:
        rid = WORK.get()
        j = JOBS[rid]
        try:
            j["state"] = "running"
            if eng is None:
                j["log"].append("building the M-vance engine (once)...")
                eng = em.MvanceEngine()

            def log(line, j=j):
                j["log"].append(line)
                m = RENDERS_RE.search(line)
                if m:
                    j["progress"] = min(int(m.group(1)) / j["budget"], 0.99)

            a = SimpleNamespace(note=j["note"], gate_ms=j["gate_ms"], budget=j["budget"], seed=1, workers=workers)
            folder = RUNS / rid
            r = match.run_one(folder / j["input"], folder, a, eng, log=log)
            j["result"] = {"score": round(r["loss"], 2), "d1": round(r["d1"], 2), "verdict": r["verdict"], "syx": r["syx"],
                           "renders": r["renders"], "seconds": round(r["seconds"])}
            (folder / "result.json").write_text(json.dumps(job_view(j) | {"state": "done"}, indent=1))
            j["state"], j["progress"] = "done", 1.0
        except Exception as e:                          # (the page shows it; the server carries on)
            j["state"], j["error"] = "error", f"{type(e).__name__}: {e}"
            j["log"].append(traceback.format_exc())
        finally:
            WORK.task_done()


def past_runs() -> list:
    """Runs from earlier sessions (their result.json), newest first, plus this session's."""
    seen = {}
    for f in sorted(RUNS.glob("*/result.json"), reverse=True):
        try:
            d = json.loads(f.read_text())
            seen[d["id"]] = {k: d.get(k) for k in ("id", "name", "state", "result")}
        except (OSError, ValueError, KeyError):
            continue
    for rid, j in JOBS.items():
        seen[rid] = {k: j.get(k) for k in ("id", "name", "state", "result", "progress")}
    return sorted(seen.values(), key=lambda d: d["id"], reverse=True)


class Handler(BaseHTTPRequestHandler):
    """GET /, /runs, /status/<id>, /files/<id>/<name>, /editor/<file> (the bank editor, sim/bank/);
    POST /run?name=&note=&gate=&search= (body: the audio)."""

    def log_message(self, fmt, *args):                 # (quiet: the page polls every second)
        pass

    def send(self, code, body, ctype="application/json", extra=None):
        data = body if isinstance(body, bytes) else (json.dumps(body) if ctype == "application/json" else body).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        path = urlparse(self.path).path
        if path in ("/", "/index.html"):
            return self.send(200, (HERE / "match.html").read_bytes(), "text/html; charset=utf-8")
        m = re.fullmatch(r"/editor/([\w.-]*)", path)
        if m:                                           # the bank editor (its own files only)
            name = m.group(1) or "editor.html"
            if name in EDITOR_FILES and (EDITOR / name).exists():
                return self.send(200, (EDITOR / name).read_bytes(), EDITOR_FILES[name])
            return self.send(404, {"error": "not found"})
        if path == "/runs":
            return self.send(200, past_runs())
        m = re.fullmatch(r"/status/([\w-]+)", path)
        if m:
            j = JOBS.get(m.group(1))
            if j:
                return self.send(200, job_view(j) | {"waiting": WORK.qsize() if j["state"] == "queued" else 0})
            f = RUNS / m.group(1) / "result.json"
            return self.send(200, json.loads(f.read_text())) if f.exists() else self.send(404, {"error": "no run"})
        m = re.fullmatch(r"/files/([\w-]+)/([\w-]+(?:\.[\w-]+)*)", path)
        if m and (m.group(2) in FILES or m.group(2).endswith(".syx")):   # (the patch: <sample name>.syx)
            f = RUNS / m.group(1) / m.group(2)
            if f.exists():
                ctype = FILES.get(m.group(2), "application/octet-stream")
                extra = {} if m.group(2).endswith((".html", ".wav")) else \
                    {"Content-Disposition": f'attachment; filename="{m.group(2)}"'}
                return self.send(200, f.read_bytes(), ctype, extra)
        self.send(404, {"error": "not found"})

    def do_POST(self):
        u = urlparse(self.path)
        if u.path != "/run":
            return self.send(404, {"error": "not found"})
        q = {k: v[0] for k, v in parse_qs(u.query).items()}
        n = int(self.headers.get("Content-Length", 0))
        name = q.get("name", "sample.wav")
        ext = Path(name).suffix.lower()
        if n <= 0 or n > MAX_UPLOAD or ext not in AUDIO_EXT:
            return self.send(400, {"error": f"send one audio file ({', '.join(sorted(AUDIO_EXT))}), "
                                            f"up to {MAX_UPLOAD >> 20} MB"})
        rid = run_id(name)
        folder = RUNS / rid
        folder.mkdir(parents=True, exist_ok=True)
        saved = (re.sub(r"[^A-Za-z0-9 _-]+", "_", Path(name).stem).strip() or "sample") + ext   # (its stem names
        (folder / saved).write_bytes(self.rfile.read(n))                                          #  the patch)
        try:
            note = float(q["note"]) if q.get("note") else None
            gate = float(q["gate"]) if q.get("gate") else None
        except ValueError:
            return self.send(400, {"error": "note and key-held time must be numbers"})
        with LOCK:
            JOBS[rid] = {"id": rid, "name": name, "input": saved, "state": "queued", "progress": 0.0,
                         "log": [], "note": note, "gate_ms": gate, "budget": BUDGETS.get(q.get("search"), 8000),
                         "queued_at": time.strftime("%H:%M:%S")}
            WORK.put(rid)
        self.send(200, {"id": rid})


def open_browser(url: str):
    """Windows' browser when in WSL (cmd.exe start), else the default one."""
    cmd = shutil.which("cmd.exe")
    if cmd:
        subprocess.Popen([cmd, "/c", "start", "", url], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                         cwd="/mnt/c" if Path("/mnt/c").exists() else None)
    else:
        webbrowser.open(url)


def main():
    ap = argparse.ArgumentParser(description="Sound match in the browser (a local web server for match.py).")
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--host", default="127.0.0.1", help="address to listen on (default: this computer only)")
    ap.add_argument("--workers", type=int, default=None, help="CPU cores a search uses (default: all)")
    ap.add_argument("--no-open", action="store_true", help="do not open the browser")
    a = ap.parse_args()
    RUNS.mkdir(parents=True, exist_ok=True)
    threading.Thread(target=worker, args=(a.workers,), daemon=True).start()
    srv = ThreadingHTTPServer((a.host, a.port), Handler)
    url = f"http://localhost:{a.port}"
    print(f"Sound match: {url}   (runs kept in {RUNS})   Ctrl+C to stop", flush=True)
    if not a.no_open:
        open_browser(url)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("stopped")


if __name__ == "__main__":
    main()
