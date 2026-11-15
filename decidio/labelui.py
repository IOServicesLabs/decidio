"""decidio label ui: a local labelling page (http://localhost:8765). Answers go to data/labels.csv."""
from __future__ import annotations

import json
import threading
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, unquote, urlparse

from .config import Project
from .store import Store

PAGE = Path(__file__).parent / "web" / "label.html"


def serve(p: Project, port: int = 8765, open_browser: bool = True) -> None:
    st = Store(p)
    lock = threading.Lock()
    cache = {"metas": None}

    def metas():
        if cache["metas"] is None:
            cache["metas"] = st.metas()
        return cache["metas"]

    def project_json():
        return {
            "name": p.name,
            "questions": [{"id": q.qid, "type": q.qtype, "options": q.options, "text": q.option_text() if q.criteria else
                           [""] * q.n, "instructions": q.instructions, "modality": q.modality} for q in p.questions],
            "image": bool(p.image), "audio": bool(p.audio),
        }

    def listing(filt: str, qid: str | None, source: str | None):
        labels, teacher = st.labels(), st.teacher()
        out = []
        for sid, m in sorted(metas().items()):
            if source and m.source != source:
                continue
            lab = labels.get(sid, {})
            qs = [qid] if qid else [q.qid for q in p.questions]
            if filt == "unlabeled" and all(q in lab for q in qs):
                continue
            if filt == "labeled" and not any(q in lab for q in qs):
                continue
            if filt == "unsure":
                t = teacher.get(sid, {})
                if not t or all(max(t.get(q, [1])) >= 0.75 for q in qs) or all(q in lab for q in qs):
                    continue
            out.append(sid)
        return out

    class H(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def _send(self, code, body, ctype="application/json"):
            data = body if isinstance(body, bytes) else json.dumps(body).encode()
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            u = urlparse(self.path)
            qs = {k: v[0] for k, v in parse_qs(u.query).items()}
            if u.path in ("/", "/index.html"):
                return self._send(200, PAGE.read_bytes(), "text/html; charset=utf-8")
            if u.path == "/api/project":
                srcs = sorted({m.source for m in metas().values()})
                return self._send(200, {**project_json(), "sources": srcs, "total": len(metas())})
            if u.path == "/api/list":
                with lock:
                    ids = listing(qs.get("filter", "unlabeled"), qs.get("q") or None, qs.get("source") or None)
                return self._send(200, {"ids": ids})
            if u.path.startswith("/api/sample/"):
                sid = unquote(u.path.rsplit("/", 1)[1])
                img, prev, wav = st.files(sid)
                with lock:
                    lab, t = st.labels().get(sid, {}), st.teacher().get(sid, {})
                m = metas().get(sid)
                return self._send(200, {"id": sid, "labels": lab, "teacher": t, "image": img is not None,
                                        "prev": prev is not None, "audio": wav is not None,
                                        "source": m.source if m else "", "boxes": m.boxes if m else []})
            if u.path.startswith("/media/"):
                name = unquote(u.path[len("/media/"):])
                f = (p.samples / name).resolve()
                if p.samples.resolve() not in f.parents or not f.exists():
                    return self._send(404, {"error": "not found"})
                ctype = "audio/wav" if f.suffix == ".wav" else "image/png"
                return self._send(200, f.read_bytes(), ctype)
            self._send(404, {"error": "not found"})

        def do_POST(self):
            if self.path != "/api/label":
                return self._send(404, {"error": "not found"})
            body = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))) or b"{}")
            sid, labels = body.get("id"), body.get("labels", {})
            qm = p.qmap()
            clean = {}
            for k, v in labels.items():
                if k in qm:
                    if v in (None, ""):
                        clean[k] = ""
                    else:
                        p.option_index(qm[k], v)
                        clean[k] = v
            with lock:
                st.update_labels({sid: clean})
            self._send(200, {"ok": True})

    srv = ThreadingHTTPServer(("127.0.0.1", port), H)
    url = f"http://localhost:{port}/"
    print(f"the label page for {p.name} is at {url}  (Ctrl+C to stop)")
    if open_browser:
        threading.Timer(0.5, lambda: webbrowser.open(url)).start()
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        srv.server_close()
