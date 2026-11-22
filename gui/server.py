#!/usr/bin/env python3
"""
Smart Campus Print Spooler - Web GUI & IPC Server
Serves the web dashboard, manages the C++ daemon process, and routes real-time telemetry and user job submissions.
"""

import http.server
import socketserver
import json
import os
import sys
import subprocess
import threading
import time
import urllib.parse
import webbrowser
from collections import deque

PORT = 8080
BASE_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_DIR = os.path.dirname(BASE_DIR)

# Global simulation state
class SpoolerDaemonManager:
    def __init__(self):
        self.lock = threading.Lock()
        self.proc = None
        self.is_running = False
        self.is_paused = False
        self.sim_time = 0.0
        self.latest_telemetry = None
        self.recent_logs = deque(maxlen=400)
        self.gantt_events = []
        self.next_auto_id = 1
        self.staged_jobs = []
        self.wsl_dir = None
        self.subscribers = []

    def get_wsl_path(self, win_path):
        try:
            res = subprocess.run(["wsl", "-d", "Ubuntu-24.04", "wslpath", "-u", win_path],
                                 capture_output=True, text=True, check=True)
            return res.stdout.strip()
        except Exception:
            # Fallback drive conversion
            p = win_path.replace("\\", "/")
            if len(p) >= 2 and p[1] == ":":
                drive = p[0].lower()
                return f"/mnt/{drive}{p[2:]}"
            return p

    def start_daemon(self, num_printers=3, page_time=0.2, realtime=False, staged_jobs=None):
        with self.lock:
            if self.is_running and self.proc:
                return {"success": False, "error": "Daemon is already running"}

            self.wsl_dir = self.get_wsl_path(PROJECT_DIR)
            args = ["./print_spooler", "--ipc", "--printers", str(num_printers)]
            if page_time > 0:
                args.extend(["--page-time", str(page_time)])
            if realtime:
                args.append("--realtime")

            cmd_str = f"cd '{self.wsl_dir}' && " + " ".join(args)
            full_cmd = ["wsl", "-d", "Ubuntu-24.04", "bash", "-c", cmd_str]

            try:
                self.proc = subprocess.Popen(
                    full_cmd,
                    stdin=subprocess.PIPE,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                    bufsize=1
                )
            except Exception as e:
                return {"success": False, "error": f"Failed to spawn daemon: {e}"}

            self.is_running = True
            self.is_paused = False
            self.gantt_events = []
            self.recent_logs.clear()
            self.latest_telemetry = None

            # Reader thread for stdout
            threading.Thread(target=self._reader_loop, daemon=True).Thread = None
            t = threading.Thread(target=self._reader_loop, daemon=True)
            t.start()

            # Feeder for staged jobs
            if staged_jobs:
                threading.Thread(target=self._feed_staged_jobs, args=(staged_jobs,), daemon=True).start()

            return {"success": True, "message": "Daemon started successfully"}

    def _feed_staged_jobs(self, jobs):
        time.sleep(0.4)
        for j in jobs:
            if not self.is_running:
                break
            self.submit_job(
                job_id=j.get("id", 0),
                client_id=j.get("client", j.get("client_id", "user")),
                role=j.get("role", "student"),
                pages=j.get("pages", 3),
                urgency=j.get("urgency", False),
                duplex=j.get("duplex", False),
                doc_name=j.get("doc_name", "")
            )
            time.sleep(0.05)

    def _reader_loop(self):
        while self.is_running and self.proc:
            line = self.proc.stdout.readline()
            if not line:
                break
            line = line.strip()
            if not line:
                continue

            try:
                data = json.loads(line)
                msg_type = data.get("type", "")

                with self.lock:
                    if msg_type == "telemetry":
                        self.latest_telemetry = data
                        self.sim_time = data.get("sim_time", self.sim_time)
                    elif msg_type == "log":
                        self.recent_logs.append(data)
                    elif msg_type == "gantt":
                        self.gantt_events.append(data)
                    elif msg_type == "pause_ack":
                        self.is_paused = True
                    elif msg_type == "resume_ack":
                        self.is_paused = False
                    elif msg_type == "stopped":
                        self.is_running = False
                        break

                # Broadcast to SSE subscribers
                self._notify_subscribers(line)
            except json.JSONDecodeError:
                pass

        with self.lock:
            self.is_running = False
            self.proc = None

    def _notify_subscribers(self, message):
        dead = []
        for q in self.subscribers:
            try:
                q.put_nowait(message)
            except Exception:
                dead.append(q)
        for d in dead:
            if d in self.subscribers:
                self.subscribers.remove(d)

    def submit_job(self, job_id, client_id, role, pages, urgency, duplex, doc_name):
        with self.lock:
            if not self.is_running or not self.proc:
                return {"success": False, "error": "Daemon is not running"}

            if not job_id or job_id <= 0:
                job_id = self.next_auto_id
                self.next_auto_id += 1
            else:
                if job_id >= self.next_auto_id:
                    self.next_auto_id = job_id + 1

            client_id = client_id.strip() or f"client_{job_id}"
            role = role.lower()
            if role not in ["student", "researcher", "faculty"]:
                role = "student"
            pages = max(1, min(100, int(pages)))
            urg_val = 1 if urgency else 0
            dup_val = 1 if duplex else 0
            doc_name = (doc_name.strip() or f"doc_{job_id}.pdf").replace(" ", "_")

            cmd = f"SUBMIT {job_id} {client_id} {role} {pages} {urg_val} {dup_val} {doc_name}\n"
            try:
                self.proc.stdin.write(cmd)
                self.proc.stdin.flush()
                return {"success": True, "job_id": job_id, "message": f"Job #{job_id} queued"}
            except Exception as e:
                return {"success": False, "error": str(e)}

    def pause_daemon(self):
        with self.lock:
            if not self.is_running or not self.proc:
                return {"success": False, "error": "Daemon is not running"}
            try:
                self.proc.stdin.write("PAUSE\n")
                self.proc.stdin.flush()
                self.is_paused = True
                return {"success": True}
            except Exception as e:
                return {"success": False, "error": str(e)}

    def resume_daemon(self):
        with self.lock:
            if not self.is_running or not self.proc:
                return {"success": False, "error": "Daemon is not running"}
            try:
                self.proc.stdin.write("RESUME\n")
                self.proc.stdin.flush()
                self.is_paused = False
                return {"success": True}
            except Exception as e:
                return {"success": False, "error": str(e)}

    def step_daemon(self, delta=0.1):
        with self.lock:
            if not self.is_running or not self.proc:
                return {"success": False, "error": "Daemon is not running"}
            try:
                self.proc.stdin.write(f"STEP {delta}\n")
                self.proc.stdin.flush()
                return {"success": True}
            except Exception as e:
                return {"success": False, "error": str(e)}

    def set_speed(self, speed=1.0):
        with self.lock:
            if not self.is_running or not self.proc:
                return {"success": False, "error": "Daemon is not running"}
            try:
                self.proc.stdin.write(f"SPEED {speed}\n")
                self.proc.stdin.flush()
                return {"success": True}
            except Exception as e:
                return {"success": False, "error": str(e)}

    def stop_daemon(self):
        with self.lock:
            if not self.is_running or not self.proc:
                return {"success": False, "error": "Daemon is not running"}
            try:
                self.proc.stdin.write("STOP\n")
                self.proc.stdin.flush()
                return {"success": True}
            except Exception as e:
                return {"success": False, "error": str(e)}

    def get_state(self):
        with self.lock:
            return {
                "running": self.is_running,
                "paused": self.is_paused,
                "sim_time": self.sim_time,
                "telemetry": self.latest_telemetry,
                "logs": list(self.recent_logs)[-80:],
                "gantt": self.gantt_events[-150:]
            }

daemon_mgr = SpoolerDaemonManager()

# Pre-configured test scenario presets
PRESETS = {
    "campus_normal": {
        "title": "Campus Daily Operations",
        "description": "Standard mix of students printing course assignments, researchers printing drafts, and faculty lecture materials.",
        "printers": 3,
        "page_time": 0.2,
        "jobs": [
            {"id": 1, "client": "faculty_prof_sen", "role": "faculty", "pages": 35, "urgency": True, "duplex": True, "doc_name": "Syllabus_Fall2026.pdf"},
            {"id": 2, "client": "student_alex", "role": "student", "pages": 3, "urgency": False, "duplex": False, "doc_name": "Lab_Report_1.pdf"},
            {"id": 3, "client": "researcher_elena", "role": "researcher", "pages": 15, "urgency": False, "duplex": True, "doc_name": "Neural_Arch_Draft.pdf"},
            {"id": 4, "client": "student_maya", "role": "student", "pages": 2, "urgency": False, "duplex": False, "doc_name": "Assignment_Math.pdf"},
            {"id": 5, "client": "student_rahul", "role": "student", "pages": 1, "urgency": False, "duplex": False, "doc_name": "Fee_Receipt.pdf"},
            {"id": 6, "client": "faculty_dean", "role": "faculty", "pages": 50, "urgency": False, "duplex": True, "doc_name": "Annual_Faculty_Report.pdf"},
            {"id": 7, "client": "student_kavya", "role": "student", "pages": 5, "urgency": True, "duplex": False, "doc_name": "Project_Abstract.pdf"},
            {"id": 8, "client": "researcher_dev", "role": "researcher", "pages": 22, "urgency": False, "duplex": True, "doc_name": "Survey_Systems.pdf"},
            {"id": 9, "client": "student_sam", "role": "student", "pages": 4, "urgency": False, "duplex": False, "doc_name": "Resume_v2.pdf"},
            {"id": 10, "client": "faculty_gupta", "role": "faculty", "pages": 18, "urgency": False, "duplex": True, "doc_name": "Midterm_Quiz_Copies.pdf"}
        ]
    },
    "tc01_starvation": {
        "title": "TC-01: Starvation Elimination via Priority Aging",
        "description": "Submits a heavy 80-page Faculty job alongside continuous 1-page student jobs. Watch dynamic aging accumulate until the faculty job overtakes student jobs!",
        "printers": 2,
        "page_time": 0.2,
        "jobs": [
            {"id": 1, "client": "faculty_prof_long", "role": "faculty", "pages": 80, "urgency": False, "duplex": True, "doc_name": "Heavy_Monograph_80p.pdf"},
            {"id": 2, "client": "student_01", "role": "student", "pages": 1, "urgency": False, "duplex": False, "doc_name": "QuickTicket_1.pdf"},
            {"id": 3, "client": "student_02", "role": "student", "pages": 1, "urgency": False, "duplex": False, "doc_name": "QuickTicket_2.pdf"},
            {"id": 4, "client": "student_03", "role": "student", "pages": 1, "urgency": False, "duplex": False, "doc_name": "QuickTicket_3.pdf"},
            {"id": 5, "client": "student_04", "role": "student", "pages": 1, "urgency": False, "duplex": False, "doc_name": "QuickTicket_4.pdf"},
            {"id": 6, "client": "student_05", "role": "student", "pages": 1, "urgency": False, "duplex": False, "doc_name": "QuickTicket_5.pdf"},
            {"id": 7, "client": "student_06", "role": "student", "pages": 1, "urgency": False, "duplex": False, "doc_name": "QuickTicket_6.pdf"},
            {"id": 8, "client": "student_07", "role": "student", "pages": 1, "urgency": False, "duplex": False, "doc_name": "QuickTicket_7.pdf"}
        ]
    },
    "tc03_swap_stress": {
        "title": "TC-03: First-Fit 64 KB Arena & LRU Swap Stress",
        "description": "Submits multiple large documents that exceed the 64 KB RAM arena, forcing the LRU Swap Engine to evict resident jobs to swap.img via SCAN disk scheduling.",
        "printers": 2,
        "page_time": 0.4,
        "jobs": [
            {"id": 1, "client": "faculty_large1", "role": "faculty", "pages": 16, "urgency": False, "duplex": True, "doc_name": "Curriculum_16KB.pdf"},
            {"id": 2, "client": "faculty_large2", "role": "faculty", "pages": 16, "urgency": False, "duplex": True, "doc_name": "Proceedings_16KB.pdf"},
            {"id": 3, "client": "faculty_large3", "role": "faculty", "pages": 16, "urgency": False, "duplex": True, "doc_name": "Handbook_16KB.pdf"},
            {"id": 4, "client": "faculty_large4", "role": "faculty", "pages": 16, "urgency": False, "duplex": True, "doc_name": "Accreditation_16KB.pdf"},
            {"id": 5, "client": "urgent_dean", "role": "faculty", "pages": 16, "urgency": True, "duplex": True, "doc_name": "URGENT_Crisis_Report_16KB.pdf"}
        ]
    }
}

class SpoolerHTTPRequestHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, format, *args):
        # Silence standard HTTP access logging to keep terminal tidy
        pass

    def do_GET(self):
        url = urllib.parse.urlparse(self.path)
        path = url.path

        if path == "/" or path == "/index.html":
            html_path = os.path.join(BASE_DIR, "index.html")
            if os.path.exists(html_path):
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.end_headers()
                with open(html_path, "rb") as f:
                    self.wfile.write(f.read())
            else:
                self.send_error(404, "index.html not found")
            return

        elif path == "/api/state":
            state = daemon_mgr.get_state()
            self._send_json(state)
            return

        elif path == "/api/presets":
            self._send_json(PRESETS)
            return

        elif path == "/api/export_gantt":
            csv_path = os.path.join(PROJECT_DIR, "gantt_timeline.csv")
            if os.path.exists(csv_path):
                self.send_response(200)
                self.send_header("Content-Type", "text/csv")
                self.send_header("Content-Disposition", 'attachment; filename="gantt_timeline.csv"')
                self.end_headers()
                with open(csv_path, "rb") as f:
                    self.wfile.write(f.read())
            else:
                self._send_json({"error": "No Gantt CSV generated yet"}, status=404)
            return

        else:
            super().do_GET()

    def do_POST(self):
        url = urllib.parse.urlparse(self.path)
        path = url.path
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length).decode("utf-8") if length > 0 else "{}"
        try:
            payload = json.loads(body)
        except Exception:
            payload = {}

        if path == "/api/start":
            res = daemon_mgr.start_daemon(
                num_printers=payload.get("printers", 3),
                page_time=payload.get("page_time", 0.2),
                realtime=payload.get("realtime", False),
                staged_jobs=payload.get("staged_jobs", None)
            )
            self._send_json(res)

        elif path == "/api/submit":
            res = daemon_mgr.submit_job(
                job_id=payload.get("id", 0),
                client_id=payload.get("client", payload.get("client_id", "")),
                role=payload.get("role", "student"),
                pages=payload.get("pages", 1),
                urgency=payload.get("urgency", False),
                duplex=payload.get("duplex", False),
                doc_name=payload.get("doc_name", "")
            )
            self._send_json(res)

        elif path == "/api/batch":
            jobs = payload.get("jobs", [])
            results = []
            for j in jobs:
                r = daemon_mgr.submit_job(
                    job_id=j.get("id", 0),
                    client_id=j.get("client", j.get("client_id", "")),
                    role=j.get("role", "student"),
                    pages=j.get("pages", 1),
                    urgency=j.get("urgency", False),
                    duplex=j.get("duplex", False),
                    doc_name=j.get("doc_name", "")
                )
                results.append(r)
            self._send_json({"success": True, "count": len(results), "results": results})

        elif path == "/api/pause":
            res = daemon_mgr.pause_daemon()
            self._send_json(res)

        elif path == "/api/resume":
            res = daemon_mgr.resume_daemon()
            self._send_json(res)

        elif path == "/api/step":
            res = daemon_mgr.step_daemon(payload.get("delta", 0.1))
            self._send_json(res)

        elif path == "/api/speed":
            res = daemon_mgr.set_speed(payload.get("speed", 1.0))
            self._send_json(res)

        elif path == "/api/stop":
            res = daemon_mgr.stop_daemon()
            self._send_json(res)

        else:
            self._send_json({"error": "Unknown API endpoint"}, status=404)

    def _send_json(self, data, status=200):
        body = json.dumps(data).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(body)

def run_server(port=PORT):
    actual_port = port
    while actual_port < port + 50:
        try:
            httpd = socketserver.TCPServer(("", actual_port), SpoolerHTTPRequestHandler)
            break
        except OSError:
            actual_port += 1

    url = f"http://localhost:{actual_port}"
    print("=" * 60)
    print(" SMART CAMPUS PRINT SPOOLER - GUI DASHBOARD SERVER")
    print("=" * 60)
    print(f" Web Interface:  {url}")
    print(f" WSL Directory:  {PROJECT_DIR}")
    print(" Press Ctrl+C to shutdown server.")
    print("=" * 60)

    # Automatically open browser
    def open_browser():
        time.sleep(0.8)
        try:
            webbrowser.open(url)
        except Exception:
            pass

    threading.Thread(target=open_browser, daemon=True).start()

    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down server...")
        daemon_mgr.stop_daemon()
        httpd.server_close()

if __name__ == "__main__":
    p = PORT
    if len(sys.argv) > 1:
        try:
            p = int(sys.argv[1])
        except ValueError:
            pass
    run_server(p)
