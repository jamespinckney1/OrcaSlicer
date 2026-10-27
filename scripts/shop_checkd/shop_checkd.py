#!/usr/bin/env python3
"""Print-shop failure check service: watches the shop's printers so failure checks keep working while the
slicer is closed.

The slicer adds the checkpoints to a job's G-code as before, then hands the job to this service (the
G-code, the printer's camera calibration and where to send phone notifications). The service watches the
printer through Moonraker, photographs the plate at each checkpoint, compares it with the G-code using
the slicer's own engine (the shop_check tool) and resumes or holds the print, exactly as the slicer's
monitor does (src/slic3r/GUI/FailureCheckMonitor.cpp). A slicer that is open only shows the status and
the review window; it never answers a checkpoint of a job the service has.

HTTP API. Every request but /health needs "Authorization: Bearer <token>", the token in the data folder
(created at first start; Preferences > "Failure check service token" in the slicer). Allow only the
slicer's networks to reach the port as well: a job names the printer the service will hold and resume.
    GET  /health                       {"ok": true, "version": ...}
    POST /jobs                         JSON {id, host, api, job_name, camera, ntfy_server, ntfy_topic}
    PUT  /jobs/<id>/gcode              the job's G-code (with checkpoints); starts watching it
    GET  /status?since=<seq>           {"hosts": {host: {text, job_id, job_name}}, "events": [...], "seq": n}
    GET  /jobs/<id>/<cpN.jpg|cpN.json> a checkpoint's photo or result
    DELETE /jobs/<id>                  stop watching a job

Standard library only. Usage: shop_checkd.py --data /var/lib/shop-checkd --check /opt/shop-checkd/shop_check
"""
import argparse
import hmac
import json
import logging
import math
import os
import re
import secrets
import shutil
import subprocess
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

VERSION = "1"
KEEP_JOBS = 30            # job folders kept (G-code plus photos)
UNSTARTED_EXPIRY_S = 12 * 3600
PARK_TOLERANCE_MM = 5.0
ID_RE = re.compile(r"^[A-Za-z0-9_.-]{1,160}$")
FILE_RE = re.compile(r"^cp\d{1,3}\.(jpg|json)$")
MAX_GCODE_BYTES = 1 << 30

log = logging.getLogger("shop_checkd")


# ---- small helpers --------------------------------------------------------------------------------

def http_get(url, timeout):
    try:
        with urllib.request.urlopen(url, timeout=timeout) as r:
            return r.read() if r.status == 200 else None
    except (urllib.error.URLError, OSError, ValueError):
        return None


def http_post_json(url, obj, timeout):
    req = urllib.request.Request(url, data=json.dumps(obj).encode(), method="POST",
                                 headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status == 200
    except (urllib.error.URLError, OSError, ValueError):
        return False


def send_script(api, script):
    log.info("sending %s to %s", script, api)
    # Moonraker answers a script when it finishes; SHOP_CHECK_OK returns at once, so a short wait is enough.
    threading.Thread(target=http_post_json, args=(api + "/printer/gcode/script", {"script": script}, 600),
                     daemon=True).start()


def snapshot_url(host):
    return "http://%s/webcam/?action=snapshot" % host


def write_atomic(path, data):
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data if isinstance(data, bytes) else data.encode())
    os.replace(tmp, path)


def masked(topic):
    return (topic[:3] + "...(%d)" % len(topic)) if topic else "<none>"


# ---- phone notifications (same format as the slicer's ShopNotify) ---------------------------------

def notify(job, title, body, tags="", priority="", attachment=None):
    server = (job.get("ntfy_server") or "https://ntfy.sh").rstrip("/")
    topic = job.get("ntfy_topic") or ""
    if not re.match(r"^[A-Za-z0-9_-]{1,64}$", topic):
        return
    url = server + "/" + topic

    def query(with_body):
        q = {k: v for k, v in (("title", title), ("tags", tags), ("priority", priority)) if v}
        if with_body:
            q["message"] = body
        return "?" + urllib.parse.urlencode(q, quote_via=urllib.parse.quote) if q else ""

    def run():
        if attachment and os.path.isfile(attachment):
            with open(attachment, "rb") as f:
                data = f.read()
            req = urllib.request.Request(url + query(True), data=data, method="PUT",
                                         headers={"Filename": os.path.basename(attachment)})
            try:
                with urllib.request.urlopen(req, timeout=60):
                    return
            except (urllib.error.URLError, OSError) as e:
                log.warning("ntfy attachment to %s failed (%s), sending text only", masked(topic), e)
        req = urllib.request.Request(url + query(False), data=(body or title or " ").encode(), method="POST",
                                     headers={"Content-Type": "text/plain; charset=utf-8"})
        try:
            with urllib.request.urlopen(req, timeout=30):
                pass
        except (urllib.error.URLError, OSError) as e:
            log.warning("ntfy to %s failed: %s", masked(topic), e)

    threading.Thread(target=run, daemon=True).start()


# ---- the service ----------------------------------------------------------------------------------

class Service:
    def __init__(self, data_dir, check_bin):
        self.data = data_dir
        self.jobs_dir = os.path.join(data_dir, "jobs")
        self.check_bin = check_bin
        os.makedirs(self.jobs_dir, exist_ok=True)
        self.lock = threading.Lock()
        self.wake = threading.Condition(self.lock)
        self.jobs = {}      # id -> job dict (active jobs only)
        self.status = {}    # host -> {"text", "job_id", "job_name"}
        self.events = []    # [{"seq", "host", "job_id", "job_name", "index", "label", "mode", ...}]
        self.seq = 0
        self.load()

    # -- persistence
    def job_dir(self, job_id):
        return os.path.join(self.jobs_dir, job_id)

    def save(self, job):
        write_atomic(os.path.join(self.job_dir(job["id"]), "job.json"), json.dumps(job, indent=2))

    def load(self):
        for name in sorted(os.listdir(self.jobs_dir)):
            path = os.path.join(self.jobs_dir, name, "job.json")
            try:
                with open(path) as f:
                    job = json.load(f)
            except (OSError, ValueError):
                continue
            if job.get("active") and os.path.isfile(os.path.join(self.jobs_dir, name, "job.gcode")):
                self.jobs[job["id"]] = job
                self.status[job["host"]] = {"text": "Failure check: watching %s (service restarted)" % job["job_name"],
                                            "job_id": job["id"], "job_name": job["job_name"]}
        log.info("loaded %d active job(s)", len(self.jobs))

    def prune(self, keep_id):
        dirs = sorted(d for d in os.listdir(self.jobs_dir) if os.path.isdir(os.path.join(self.jobs_dir, d)))
        for d in dirs[:-KEEP_JOBS]:
            if d != keep_id and d not in self.jobs:
                shutil.rmtree(os.path.join(self.jobs_dir, d), ignore_errors=True)

    # -- state shared with the HTTP side (call with the lock held)
    def set_status(self, job, text):
        self.status[job["host"]] = {"text": text, "job_id": job["id"], "job_name": job["job_name"]}
        log.info("%s: %s", job["host"], text)

    def add_event(self, job, index, label, mode, reasons=None, review_reason=""):
        self.seq += 1
        self.events.append({"seq": self.seq, "host": job["host"], "job_id": job["id"], "job_name": job["job_name"],
                            "index": index, "label": label, "mode": mode, "total": job["total"] - 1,
                            "reasons": reasons or [], "review_reason": review_reason, "time": int(time.time())})
        del self.events[:-50]

    # -- jobs from the slicer
    def create_job(self, meta):
        job_id = meta.get("id", "")
        if not ID_RE.match(job_id) or not meta.get("host") or not meta.get("api") or not meta.get("job_name"):
            raise ValueError("id, host, api and job_name are required")
        d = self.job_dir(job_id)
        os.makedirs(d, exist_ok=True)
        if isinstance(meta.get("camera"), dict):
            write_atomic(os.path.join(d, "camera.json"), json.dumps(meta["camera"], indent=2))
        job = {"id": job_id, "host": meta["host"], "api": meta["api"].rstrip("/"), "job_name": meta["job_name"],
               "ntfy_server": meta.get("ntfy_server", ""), "ntfy_topic": meta.get("ntfy_topic", ""),
               "created": int(time.time()), "active": False, "started": False, "handled": [], "has_baseline": False,
               "baseline_z": 0.0, "total": 0, "passed": 0, "alignment": [], "camera_warned": False, "checkpoints": {},
               "restarts": -1}
        with self.lock:
            self.save(job)
        return job

    def attach_gcode(self, job_id, stream, length):
        d = self.job_dir(job_id)
        try:
            with open(os.path.join(d, "job.json")) as f:
                job = json.load(f)
        except (OSError, ValueError):
            raise KeyError(job_id)
        tmp = os.path.join(d, "job.gcode.tmp")
        with open(tmp, "wb") as f:
            left = length
            while left > 0:
                chunk = stream.read(min(left, 1 << 20))
                if not chunk:
                    raise ValueError("upload ended early")
                f.write(chunk)
                left -= len(chunk)
        os.replace(tmp, os.path.join(d, "job.gcode"))
        info = self.run_check(["info", os.path.join(d, "job.gcode")])
        if not info or "checkpoints" not in info or len(info["checkpoints"]) < 2:
            raise ValueError("the G-code has no failure-check checkpoints")
        job["checkpoints"] = {str(c["index"]): {"kind": c["kind"], "label": c["label"]} for c in info["checkpoints"]}
        job["total"] = len(info["checkpoints"])
        job["active"] = True
        with self.lock:
            for other in [j for j in self.jobs.values() if j["host"] == job["host"]]:
                other["active"] = False
                self.save(other)
                del self.jobs[other["id"]]
            self.jobs[job_id] = job
            self.save(job)
            self.set_status(job, "Failure check: %d checkpoints in %s, waiting for the print to start"
                            % (job["total"] - 1, job["job_name"]))
            self.prune(job_id)
            self.wake.notify_all()
        return job

    def stop_job(self, job_id):
        with self.lock:
            job = self.jobs.pop(job_id, None)
            if job:
                job["active"] = False
                self.save(job)
                self.set_status(job, "Failure check: stopped watching %s" % job["job_name"])
            return job is not None

    # -- the comparison engine
    def run_check(self, args):
        try:
            out = subprocess.run([self.check_bin] + args, capture_output=True, timeout=120, text=True)
            return json.loads(out.stdout.strip().splitlines()[-1]) if out.stdout.strip() else None
        except (OSError, subprocess.TimeoutExpired, ValueError, IndexError) as e:
            log.error("shop_check %s failed: %s", args[0], e)
            return None

    # -- watching
    def run(self):
        while True:
            with self.lock:
                jobs = [dict(j) for j in self.jobs.values()]
            for job in jobs:
                try:
                    keep = self.poll(job)
                except Exception:  # one bad poll must never end the watcher
                    log.exception("polling %s failed", job["host"])
                    keep = True
                with self.lock:
                    if job["id"] not in self.jobs:
                        continue  # replaced or stopped meanwhile
                    if not keep:
                        job["active"] = False
                        del self.jobs[job["id"]]
                    else:
                        self.jobs[job["id"]] = job
                    self.save(job)
            with self.lock:
                self.wake.wait(2 if jobs else 10)

    def poll(self, job):
        if not job["started"] and time.time() - job["created"] > UNSTARTED_EXPIRY_S:
            with self.lock:
                self.status.pop(job["host"], None)
            return False
        q = urllib.parse.quote("gcode_macro SHOP_CHECKPOINT")
        r = urllib.parse.quote("gcode_macro SHOP_RESTART_POINT")
        body = http_get(job["api"] + "/printer/objects/query?print_stats=state,filename&toolhead=position&"
                        + q + "=waiting,index,held&" + r + "=restarts", 5)
        if body is None:
            return True  # printer unreachable: try again
        try:
            st = json.loads(body)["result"]["status"]
            state = st["print_stats"].get("state", "")
            filename = st["print_stats"].get("filename", "")
            macro = st.get("gcode_macro SHOP_CHECKPOINT", {})
            waiting, index = int(macro.get("waiting", 0)), int(macro.get("index", -1))
            restarts = int(st.get("gcode_macro SHOP_RESTART_POINT", {}).get("restarts", -1))
            z = float(st.get("toolhead", {}).get("position", [0, 0, 0])[2])
        except (ValueError, KeyError, TypeError, IndexError):
            return True
        ours = os.path.basename(filename) == job["job_name"]
        active = ours and state in ("printing", "paused")
        if active and not job["started"]:
            job["started"] = True
            with self.lock:
                self.set_status(job, "Failure check: watching %s" % job["job_name"])
        if job["started"] and not active:
            with self.lock:
                self.set_status(job, "Failure check: %s finished (%d of %d checks passed)"
                                % (job["job_name"], job["passed"], max(0, job["total"] - 1)))
            return False
        # The printer counts quick restarts (docs/shop/plus4/quick_restart.cfg); one since the last poll means the
        # job runs again from layer 1, with every checkpoint (the reference photo first) still to come.
        if active and restarts >= 0:
            if job.get("restarts", -1) >= 0 and restarts > job["restarts"]:
                self.restart_job(job)
            job["restarts"] = restarts
        if active and waiting == 1 and index >= 0 and index not in job["handled"]:
            self.handle_checkpoint(job, index, z)
        return True

    def restart_job(self, job):
        # The earlier attempt's photos and results move to attemptN/, so cpN.* are always the current attempt's.
        d = self.job_dir(job["id"])
        n = 1
        while os.path.exists(os.path.join(d, "attempt%d" % n)):
            n += 1
        archive = os.path.join(d, "attempt%d" % n)
        for name in os.listdir(d):
            if FILE_RE.match(name):
                os.makedirs(archive, exist_ok=True)
                os.replace(os.path.join(d, name), os.path.join(archive, name))
        job.update({"handled": [], "has_baseline": False, "baseline_z": 0.0, "passed": 0, "alignment": [],
                    "camera_warned": False})
        job.pop("reference", None)
        with self.lock:
            self.set_status(job, "Failure check: quick restart of %s, checking again from layer 1" % job["job_name"])

    def camera(self, job):
        try:
            with open(os.path.join(self.job_dir(job["id"]), "camera.json")) as f:
                return json.load(f)
        except (OSError, ValueError):
            return None

    def park_state(self, job):
        """"parked" while the checkpoint holds the printer at the park position (from the camera calibration),
        "resumed" once it no longer waits, "elsewhere" when it waits but the toolhead never parked (Klipper drops
        the park moves of a checkpoint inside an excluded object's G-code)."""
        cam = self.camera(job) or {}
        px, py = float(cam.get("park_x", 95.0)), float(cam.get("park_y", 324.0))
        q = urllib.parse.quote("gcode_macro SHOP_CHECKPOINT")
        body = http_get(job["api"] + "/printer/objects/query?toolhead=position&" + q + "=waiting", 5)
        if body is None:
            return "parked"  # cannot tell: keep the slicer's behaviour
        try:
            st = json.loads(body)["result"]["status"]
            pos = st["toolhead"]["position"]
            if int(st["gcode_macro SHOP_CHECKPOINT"].get("waiting", 0)) != 1:
                return "resumed"
            return "parked" if math.hypot(pos[0] - px, pos[1] - py) < PARK_TOLERANCE_MM else "elsewhere"
        except (ValueError, KeyError, TypeError, IndexError):
            return "parked"

    def snapshot(self, job):
        """A snapshot, restarting the printer's camera service once when it does not answer (its
        mjpg-streamer can keep running with a dead capture)."""
        jpeg = http_get(snapshot_url(job["host"]), 10)
        if jpeg:
            return jpeg
        restarted = http_post_json(job["api"] + "/machine/services/restart?service=webcamd", {}, 30)
        log.warning("camera on %s did not answer; webcamd restart %s", job["host"], "requested" if restarted else "FAILED")
        if not restarted:
            return None
        for _ in range(10):
            time.sleep(2)
            jpeg = http_get(snapshot_url(job["host"]), 5)
            if jpeg:
                return jpeg
        return None

    def previous_reference(self, job):
        """The empty-plate photo (cp0) of the most recent earlier job that has one, with its Z."""
        for d in sorted(os.listdir(self.jobs_dir), reverse=True):
            if d >= job["id"]:
                continue
            cp0, cp1 = os.path.join(self.jobs_dir, d, "cp0.jpg"), os.path.join(self.jobs_dir, d, "cp1.json")
            try:
                with open(cp1) as f:
                    z = float(json.load(f)["z"])
            except (OSError, ValueError, KeyError, TypeError):
                continue
            if os.path.isfile(cp0):
                return cp0, z
        return None, 0.0

    def handle_checkpoint(self, job, index, z):
        job["handled"].append(index)
        cp = job["checkpoints"].get(str(index))
        if cp is None:
            send_script(job["api"], "SHOP_CHECK_OK")
            return
        label, d = cp["label"], self.job_dir(job["id"])
        # Give the comparison a fresh minute before the printer's own timeout resumes the print.
        send_script(job["api"], "UPDATE_DELAYED_GCODE ID=_SHOP_CHECK_TIMEOUT DURATION=60")
        jpeg = self.snapshot(job)
        if not jpeg:
            send_script(job["api"], "SHOP_CHECK_OK")  # fail open
            with self.lock:
                self.set_status(job, 'Failure check: camera did not answer at "%s"; print continues' % label)
            if not job["camera_warned"]:
                job["camera_warned"] = True
                notify(job, "Camera not answering: %s" % job["job_name"],
                       'The camera on %s did not answer at "%s", even after restarting it. The print continues without '
                       "failure checks; power-cycling the printer usually brings the camera back." % (job["host"], label),
                       tags="warning")
            return
        parked = self.park_state(job)
        if parked == "resumed":
            with self.lock:
                self.set_status(job, 'Failure check: "%s" missed (the print had already resumed)' % label)
            return
        if parked == "elsewhere":
            send_script(job["api"], "SHOP_CHECK_OK")  # the toolhead hides the print: fail open
            with self.lock:
                self.set_status(job, 'Failure check: "%s" skipped (the toolhead did not park); print continues' % label)
            notify(job, "Check skipped: %s" % job["job_name"],
                   'The printer paused for "%s" but the toolhead did not park, so the plate could not be photographed. '
                   "This happens when the checkpoint falls inside an object excluded during the print. The print "
                   "continues." % label, tags="warning")
            return
        photo = os.path.join(d, "cp%d.jpg" % index)
        write_atomic(photo, jpeg)

        if cp["kind"] == "baseline":
            job["has_baseline"], job["baseline_z"] = True, z
            send_script(job["api"], "SHOP_CHECK_OK")
            with self.lock:
                self.set_status(job, "Failure check: reference photo taken for %s" % job["job_name"])
            return

        base, base_z, stand_in, base_index = os.path.join(d, "cp0.jpg"), job["baseline_z"], False, 0
        ref = job.get("reference")  # a first-layer photo that replaced a reference which no longer matched
        if ref and os.path.isfile(os.path.join(d, ref["photo"])):
            base, base_z, base_index = os.path.join(d, ref["photo"]), ref["z"], ref["index"]
        elif not (job["has_baseline"] and os.path.isfile(base)):
            base, base_z = self.previous_reference(job)
            stand_in = base is not None
        first_layer = cp["kind"] in ("early", "first_layer")
        prior = [] if first_layer else job["alignment"]
        res = None
        if base is not None and os.path.isfile(os.path.join(d, "camera.json")):
            args = ["compare", os.path.join(d, "job.gcode"), os.path.join(d, "camera.json"), str(index), base,
                    repr(base_z), photo, repr(z)]
            if prior:
                al = os.path.join(d, "alignment.json")
                write_atomic(al, json.dumps(prior))
                args.append(al)
            if base_index:
                args += ["--base-checkpoint", str(base_index)]
            res = self.run_check(args)
        review_reason = ""
        if base is None:
            review_reason = "this job has no reference photo of the empty plate"
        elif not os.path.isfile(os.path.join(d, "camera.json")):
            review_reason = "the camera is not calibrated yet"
        elif res is None or "error" in res:
            review_reason = "the comparison could not run" + (": " + res["error"] if res and "error" in res else "")
        elif "review_reason" in res:
            review_reason = res["review_reason"]
        if review_reason:
            # Nobody to review it while the slicer is closed: let the print go on and keep the photo.
            send_script(job["api"], "SHOP_CHECK_OK")
            with self.lock:
                self.set_status(job, 'Failure check: "%s" not compared (%s); print continues' % (label, review_reason))
                self.add_event(job, index, label, "review", review_reason=review_reason)
            return

        if stand_in:
            res["reasons"].append("compared with the previous job's empty-plate photo (this job has no reference photo)")
        # The view around the bed changed (a hand in the reference, say): the verdict is unsure. During the first
        # layer, when the plate holds little material, this photo becomes the reference for the rest of the job.
        replaced = res.get("reference_mismatch") and first_layer
        if replaced:
            job["reference"] = {"photo": os.path.basename(photo), "z": z, "index": index}
            res["reasons"].append("later checkpoints compare with this photo instead")
        if first_layer and res.get("alignment"):
            job["alignment"] = res["alignment"]
        write_atomic(os.path.join(d, "cp%d.json" % index), json.dumps(
            {"checkpoint": index, "label": label, "z": z, "unexpected_mm2": res["unexpected_mm2"],
             "missing_fraction": res["missing_fraction"], "reasons": res["reasons"], "verdict": res["verdict"],
             "base": os.path.relpath(base, self.jobs_dir), "base_z": base_z, "base_checkpoint": base_index,
             "prior_alignment": prior, "scene_change": res.get("scene_change", 0.0),
             "reference_mismatch": bool(res.get("reference_mismatch"))}, indent=2))
        if res["verdict"] == "fail":
            send_script(job["api"], "SHOP_CHECK_HOLD")
            with self.lock:
                self.set_status(job, 'Failure check: "%s" FAILED, printer paused' % label)
                self.add_event(job, index, label, "hold", reasons=res["reasons"])
            notify(job, "Print held: %s" % job["job_name"],
                   'Checkpoint "%s" (%d of %d) failed on %s%s\nThe printer is paused and beeping; the hotend turns off '
                   "after 15 minutes." % (label, index, job["total"] - 1, job["host"],
                                          (": " + res["reasons"][0]) if res["reasons"] else ""),
                   tags="warning", priority="high", attachment=photo)
            return
        job["passed"] += 1
        send_script(job["api"], "SHOP_CHECK_OK")
        if replaced:
            notify(job, "Reference photo replaced: %s" % job["job_name"],
                   'At "%s" the view around the bed no longer matched the reference photo (a hand or something else was in '
                   "view when it was taken, or the light changed), so that checkpoint was not judged. The print continues, "
                   "and later checkpoints compare with this photo." % label, tags="camera", attachment=photo)
        with self.lock:
            if res["verdict"] == "pass":
                self.set_status(job, 'Failure check: "%s" passed (%d of %d)%s' % (
                    label, job["passed"], job["total"] - 1, " (reference: previous job's photo)" if stand_in else ""))
            else:
                self.set_status(job, 'Failure check: "%s" uncertain (%s), print continues'
                                % (label, res["reasons"][0] if res["reasons"] else ""))


# ---- HTTP -----------------------------------------------------------------------------------------

def load_token(path):
    """The API token, created (owner-only) on first start."""
    if not os.path.isfile(path):
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "w") as f:
            f.write(secrets.token_urlsafe(24) + "\n")
    with open(path) as f:
        return f.read().strip()


def make_handler(svc, token):
    class Handler(BaseHTTPRequestHandler):
        server_version = "shop_checkd/" + VERSION

        def authorized(self):
            given = self.headers.get("Authorization", "")
            if hmac.compare_digest(given.encode(), ("Bearer " + token).encode()):
                return True
            self.reply(401, {"error": "missing or wrong token"})
            return False

        def log_message(self, fmt, *args):
            log.debug("%s %s", self.address_string(), fmt % args)

        def reply(self, code, obj=None, data=None, ctype="application/json"):
            payload = data if data is not None else json.dumps(obj).encode()
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)

        def parts(self):
            u = urllib.parse.urlparse(self.path)
            return [p for p in u.path.split("/") if p], urllib.parse.parse_qs(u.query)

        def do_GET(self):
            p, q = self.parts()
            if p == ["health"]:
                return self.reply(200, {"ok": True, "version": VERSION})
            if not self.authorized():
                return
            if p == ["status"]:
                since = int(q.get("since", ["0"])[0] or 0)
                with svc.lock:
                    hosts = {h: dict(v, active=v["job_id"] in svc.jobs) for h, v in svc.status.items()}
                    return self.reply(200, {"hosts": hosts, "seq": svc.seq,
                                            "events": [e for e in svc.events if e["seq"] > since]})
            if len(p) == 3 and p[0] == "jobs" and ID_RE.match(p[1]) and FILE_RE.match(p[2]):
                path = os.path.join(svc.job_dir(p[1]), p[2])
                if os.path.isfile(path):
                    with open(path, "rb") as f:
                        return self.reply(200, data=f.read(),
                                          ctype="image/jpeg" if p[2].endswith(".jpg") else "application/json")
            return self.reply(404, {"error": "not found"})

        def do_POST(self):
            p, _ = self.parts()
            if not self.authorized():
                return
            if p != ["jobs"]:
                return self.reply(404, {"error": "not found"})
            try:
                n = int(self.headers.get("Content-Length", 0))
                meta = json.loads(self.rfile.read(n) if 0 < n < (1 << 20) else b"{}")
                job = svc.create_job(meta)
            except (ValueError, OSError) as e:
                return self.reply(400, {"error": str(e)})
            return self.reply(200, {"ok": True, "upload": "/jobs/%s/gcode" % job["id"]})

        def do_PUT(self):
            p, _ = self.parts()
            if not self.authorized():
                return
            if len(p) != 3 or p[0] != "jobs" or p[2] != "gcode" or not ID_RE.match(p[1]):
                return self.reply(404, {"error": "not found"})
            try:
                n = int(self.headers.get("Content-Length", 0))
                if not 0 < n <= MAX_GCODE_BYTES:
                    raise ValueError("bad length")
                job = svc.attach_gcode(p[1], self.rfile, n)
            except KeyError:
                return self.reply(404, {"error": "unknown job"})
            except (ValueError, OSError) as e:
                return self.reply(400, {"error": str(e)})
            return self.reply(200, {"ok": True, "checkpoints": job["total"] - 1})

        def do_DELETE(self):
            p, _ = self.parts()
            if not self.authorized():
                return
            if len(p) == 2 and p[0] == "jobs" and ID_RE.match(p[1]) and svc.stop_job(p[1]):
                return self.reply(200, {"ok": True})
            return self.reply(404, {"error": "not found"})

    return Handler


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--data", default="/var/lib/shop-checkd")
    ap.add_argument("--check", default="/opt/shop-checkd/shop_check")
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--verbose", action="store_true")
    a = ap.parse_args()
    logging.basicConfig(level=logging.DEBUG if a.verbose else logging.INFO,
                        format="%(asctime)s %(levelname)s %(message)s")
    svc = Service(a.data, a.check)
    token = load_token(os.path.join(a.data, "token"))
    threading.Thread(target=svc.run, daemon=True).start()
    httpd = ThreadingHTTPServer((a.bind, a.port), make_handler(svc, token))
    log.info("listening on %s:%d, data in %s", a.bind, a.port, a.data)
    httpd.serve_forever()


if __name__ == "__main__":
    main()
