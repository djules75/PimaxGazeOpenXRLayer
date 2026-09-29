"""Pimax gaze service.

Reads both eye cameras from BrokenEye's RAW TCP API, finds the pupil in each
frame (tools/pupil_v2.py), maps pupil position to gaze angle with the per-eye
calibration written by tools/calib_fit.py --write-calib, fuses the two eyes,
and publishes the latest gaze in a named shared-memory block that the OpenXR
layer reads.

usage:
  python gaze_service.py --calib gaze_calib.json                 # live, BrokenEye on 127.0.0.1:5555
  python gaze_service.py --calib gaze_calib.json --replay <extracted recording dir> [--schedule calib-*-schedule.csv]

Shared memory: name "Local\\PimaxGazeLayer", 88 bytes, little-endian:
  0   char[4]  magic "PGZL"
  4   u32      version (1)
  8   u32      seq        (odd while the writer is mid-update; readers retry)
  12  u32      reserved
  16  i64      sample_qpc_us   QueryPerformanceCounter time (us) of the newest camera frame used
  24  f32      yaw_deg         combined gaze, head frame, + right
  28  f32      pitch_deg       + up
  32  f32[3]   dir             unit vector, +X right +Y up -Z forward
  44  f32      confidence      0..1 (0 = no gaze; the layer must report untracked)
  48  u32      flags           bit0 left eye used, bit1 right eye used
  52  u32      reserved
  56  f32[3]   left  yaw, pitch, confidence
  68  f32[3]   right yaw, pitch, confidence
  80  i64      heartbeat_qpc_us   time of the last write (service alive check)
"""
import argparse
import bisect
import ctypes
import glob
import json
import math
import mmap
import os
import queue
import socket
import struct
import sys
import threading
import time

import cv2
import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools'))
from pupil_v2 import find_pupil_v2  # noqa: E402

_log_file = None


def say(msg):
    """Print and append to %LOCALAPPDATA%\\PimaxGazeLayer\\service.log."""
    global _log_file
    line = time.strftime('%H:%M:%S ') + msg
    print(line, flush=True)
    try:
        if _log_file is None:
            d = os.path.join(os.environ.get('LOCALAPPDATA', '.'), 'PimaxGazeLayer')
            os.makedirs(d, exist_ok=True)
            _log_file = open(os.path.join(d, 'service.log'), 'a', encoding='utf-8')
        _log_file.write(line + '\n')
        _log_file.flush()
    except OSError:
        pass


SHM_NAME = 'Local\\PimaxGazeLayer'
SHM_SIZE = 256
SHM_FMT = '<4sIIIq6fII6fq'
assert struct.calcsize(SHM_FMT) == 88

MAX_EYE_AGE_US = 80_000       # an eye estimate older than this is not fused
MIN_EYE_CONF = 0.5

# ------------------------------------------------------------------ clock
_qpc_freq = ctypes.c_longlong()
ctypes.windll.kernel32.QueryPerformanceFrequency(ctypes.byref(_qpc_freq))
_QPC_TO_US = 1e6 / _qpc_freq.value


def qpc_us():
    c = ctypes.c_longlong()
    ctypes.windll.kernel32.QueryPerformanceCounter(ctypes.byref(c))
    return int(c.value * _QPC_TO_US)


# ------------------------------------------------------------------ model
def angles_to_dir(yaw, pitch):
    y, p = math.radians(yaw), math.radians(pitch)
    return (math.sin(y) * math.cos(p), math.sin(p), -math.cos(y) * math.cos(p))


def quad_design(z):
    cols = [1.0] + list(z)
    for i in range(len(z)):
        for j in range(i, len(z)):
            cols.append(z[i] * z[j])
    return np.array(cols)


class EyeModel:
    def __init__(self, d):
        self.cam = d['camera']
        self.mu = np.array(d['mu'])
        self.sd = np.array(d['sd'])
        self.W = np.array(d['W'])
        self.bx = d['pupil_bounds']['x']
        self.by = d['pupil_bounds']['y']
        self.score_med = d.get('score_median', 1.0)
        s = d.get('shape')
        self.shape = None if s is None else (np.array(s['mu']), np.array(s['sd']), np.array(s['W']))
        # Range of pupil positions the calibration actually saw (bounds carry an 8 px margin).
        r = d.get('pupil_range')
        self.lo = np.array([r['x'][0], r['y'][0]]) if r else np.array([self.bx[0] + 8, self.by[0] + 8])
        self.hi = np.array([r['x'][1], r['y'][1]]) if r else np.array([self.bx[1] - 8, self.by[1] - 8])
        # Yaw sensitivity at the calibration centre, the reference for the fold test.
        self.g0 = self._grad(*self.mu)[0]

    def _quad(self, cx, cy):
        x, y = (np.array([cx, cy]) - self.mu) / self.sd
        X = np.array([1, x, y, x * x, x * y, y * y])
        return X @ self.W

    def _grad(self, cx, cy, e=0.5):
        """Jacobian rows: d(yaw)/d(cx,cy), d(pitch)/d(cx,cy)."""
        gx = (self._quad(cx + e, cy) - self._quad(cx - e, cy)) / (2 * e)
        gy = (self._quad(cx, cy + e) - self._quad(cx, cy - e)) / (2 * e)
        return np.array([[gx[0], gy[0]], [gx[1], gy[1]]])

    def evaluate(self, cx, cy):
        """(yaw, pitch, fold_weight, px_outside). Inside the calibrated pupil range the
        quadratic is used as is; outside it the map continues in a straight line from
        the edge instead of curving. fold_weight falls from 1 towards 0 where the map's
        yaw sensitivity along its centre direction has collapsed (the eye has turned
        away from its camera and the quadratic is folding over)."""
        p = np.array([cx, cy])
        pb = np.clip(p, self.lo, self.hi)
        val = self._quad(*pb)
        J = self._grad(*pb)
        if np.any(p != pb):
            val = val + J @ (p - pb)
        s = float(J[0] @ self.g0) / max(float(self.g0 @ self.g0), 1e-9)
        fold = float(np.clip((s - 0.25) / 0.5, 0.0, 1.0))
        return float(val[0]), float(val[1]), fold, float(np.hypot(*(p - pb)))

    def predict(self, cx, cy):
        yaw, pitch, _, _ = self.evaluate(cx, cy)
        return yaw, pitch

    def predict_shape(self, w, h, ang_deg):
        """Gaze from the ellipse shape alone (slip-robust, noisier). None without a shape model."""
        if self.shape is None:
            return None
        mu, sd, W = self.shape
        a = math.radians(ang_deg)
        f = np.array([w, h, math.cos(2 * a), math.sin(2 * a), min(w, h) / max(max(w, h), 1e-6)])
        yaw, pitch = quad_design((f - mu) / sd) @ W
        return float(yaw), float(pitch)

    def confidence(self, cx, cy, score):
        """Detection quality, reduced gradually with distance beyond the calibrated pupil
        range (half at 20 px out, zero at 40 px), since the map is extrapolating there."""
        c = min(1.0, 0.5 + 0.5 * score / max(self.score_med, 1e-6))   # weak detections score low
        p = np.array([cx, cy])
        out = float(np.hypot(*(p - np.clip(p, self.lo, self.hi))))
        return c * max(0.0, 1.0 - out / 40.0)


# ------------------------------------------------------------------ shared memory
class SharedGaze:
    def __init__(self):
        self.m = mmap.mmap(-1, SHM_SIZE, tagname=SHM_NAME)
        self.seq = 0

    def write(self, sample_us, yaw, pitch, conf, flags, left, right):
        self.seq += 1                      # odd: writing
        self.m.seek(8); self.m.write(struct.pack('<I', self.seq))
        d = angles_to_dir(yaw, pitch)
        self.seq += 1                      # even: consistent
        payload = struct.pack(SHM_FMT, b'PGZL', 1, self.seq, 0, int(sample_us), yaw, pitch, d[0], d[1], d[2], conf,
                              flags, 0, left[0], left[1], left[2], right[0], right[1], right[2], qpc_us())
        self.m.seek(0); self.m.write(payload)


# ------------------------------------------------------------------ estimator
class Estimator:
    def __init__(self, calib, shm=None, log=None):
        self.models = {}
        for name in ('left', 'right'):
            if name in calib['eyes']:
                self.models[calib['eyes'][name]['camera']] = EyeModel(calib['eyes'][name])
        self.eye = {0: None, 1: None}      # (t_us, yaw, pitch, conf)
        self.shm = shm
        self.log = log
        self.last = None
        self.stats = {0: [0, 0], 1: [0, 0]}   # frames, detections

    offset = {0: np.zeros(2), 1: np.zeros(2)}     # pupil-space correction per eye (px), set by recentre()
    raw = {0: None, 1: None}                       # (t_us, cx, cy) of the last detection per eye
    # Automatic slip correction: a slow estimate per eye of (shape-model gaze - centre-model gaze),
    # added to the centre model's output. The shape model does not depend on where the pupil sits
    # in the image, so after a headset shift it pulls the output back over AUTO_TAU_S seconds.
    auto = True
    AUTO_TAU_S = 15.0
    AUTO_MAX_DEG = 15.0
    drift = {0: np.zeros(2), 1: np.zeros(2)}
    drift_n = {0: 0, 1: 0}

    last_img = {0: None, 1: None}
    snapshot_requested = False

    def process(self, cam, img, t_us):
        model = self.models.get(cam)
        if model is None:
            return
        self.stats[cam][0] += 1
        self.last_img[cam] = img
        r = find_pupil_v2(img, cam)
        if self.snapshot_requested and self.last_img[0] is not None and self.last_img[1] is not None:
            self.snapshot_requested = False
            d = os.path.join(os.environ.get('LOCALAPPDATA', '.'), 'PimaxGazeLayer')
            os.makedirs(d, exist_ok=True)
            stamp = time.strftime('%H%M%S')
            for c in (0, 1):
                im = cv2.cvtColor(self.last_img[c], cv2.COLOR_GRAY2BGR)
                rr = find_pupil_v2(self.last_img[c], c)
                if rr:
                    cv2.ellipse(im, rr[0], (0, 255, 0), 1)
                cv2.imwrite(os.path.join(d, f'snap-{stamp}-cam{c}.png'), im)
            say(f"snapshot saved: {d}\\snap-{stamp}-cam0.png / cam1.png")
        if r is None:
            self.eye[cam] = (t_us, 0.0, 0.0, 0.0, 0.0)
        else:
            (cx, cy), (w, h), ang = r[0]
            self.raw[cam] = (t_us, cx, cy)
            cx, cy = cx + self.offset[cam][0], cy + self.offset[cam][1]
            yaw, pitch, fold, _ = model.evaluate(cx, cy)
            conf = model.confidence(cx, cy, r[2])
            # Only learn the slip offset where this eye's map is well-behaved.
            if self.auto and conf >= MIN_EYE_CONF and fold > 0.8:
                gs = model.predict_shape(w, h, ang)
                if gs is not None:
                    d = np.array([gs[0] - yaw, gs[1] - pitch])
                    n = self.drift_n[cam]
                    if n < 30 or np.hypot(*(d - self.drift[cam])) < 12.0:      # settle fast, then reject outliers
                        alpha = 1.0 / (n + 1) if n < 30 else 1.0 / (self.AUTO_TAU_S * 30.0)
                        self.drift[cam] += alpha * (d - self.drift[cam])
                        self.drift_n[cam] = n + 1
                        m = np.hypot(*self.drift[cam])
                        if m > self.AUTO_MAX_DEG:
                            self.drift[cam] *= self.AUTO_MAX_DEG / m
            yaw += self.drift[cam][0]
            pitch += self.drift[cam][1]
            self.eye[cam] = (t_us, yaw, pitch, conf, fold)
            self.stats[cam][1] += 1
        self.fuse(t_us)

    def recentre(self, now_us, max_px=25.0):
        """The user is looking straight ahead: shift each eye's pupil coordinates so the
        map returns (0, 0) there. Solved in pupil space (Newton, finite-difference
        Jacobian) because the map is nonlinear. Returns a report string."""
        parts = []
        for cam, model in self.models.items():
            raw = self.raw[cam]
            if raw is None or now_us - raw[0] > 300_000:
                parts.append(f"{'LR'[cam]}: no fresh pupil"); continue
            p = np.array([raw[1], raw[2]]) + self.offset[cam]
            before = model.predict(*p)
            for _ in range(6):
                f = np.array(model.predict(*p))
                if np.hypot(*f) < 0.05:
                    break
                J = np.zeros((2, 2))
                for k in range(2):
                    dp = np.zeros(2); dp[k] = 0.5
                    J[:, k] = (np.array(model.predict(*(p + dp))) - np.array(model.predict(*(p - dp)))) / 1.0
                try:
                    p = p - np.linalg.solve(J, f)
                except np.linalg.LinAlgError:
                    break
            delta = p - np.array([raw[1], raw[2]])
            if np.hypot(*delta) > max_px:
                parts.append(f"{'LR'[cam]}: shift {np.hypot(*delta):.0f} px too large, ignored"); continue
            self.offset[cam] = delta
            parts.append(f"{'LR'[cam]}: was ({before[0]:+.1f},{before[1]:+.1f})°, offset now ({delta[0]:+.1f},{delta[1]:+.1f}) px")
        return '; '.join(parts)

    def reset_offsets(self):
        self.offset = {0: np.zeros(2), 1: np.zeros(2)}
        self.drift = {0: np.zeros(2), 1: np.zeros(2)}
        self.drift_n = {0: 0, 1: 0}
        self.auto = self.auto_enabled

    def drift_report(self):
        return "auto-offset L (%+.1f,%+.1f) R (%+.1f,%+.1f)%s" % (
            self.drift[0][0], self.drift[0][1], self.drift[1][0], self.drift[1][1], "" if self.auto else " [frozen by Ctrl+F9]")

    def fuse(self, now_us):
        used = []
        for cam in (0, 1):
            e = self.eye[cam]
            if e and now_us - e[0] <= MAX_EYE_AGE_US and e[3] >= MIN_EYE_CONF:
                used.append((cam, e))
        if used:
            c = np.array([e[3] for _, e in used])
            # An eye whose map is folding (turned away from its camera) yields to the other eye.
            w = c * np.array([max(e[4], 0.05) for _, e in used])
            yaw = float(np.dot(w, [e[1] for _, e in used]) / w.sum())
            pitch = float(np.dot(w, [e[2] for _, e in used]) / w.sum())
            conf = float(c.max() if len(used) == 1 else min(1.0, c.mean() + 0.1))
            flags = sum(1 << cam for cam, _ in used)
            sample = max(e[0] for _, e in used)
        else:
            yaw, pitch = (self.last[1], self.last[2]) if self.last else (0.0, 0.0)
            conf, flags, sample = 0.0, 0, now_us
        L = self.eye[0][1:4] if self.eye[0] else (0.0, 0.0, 0.0)
        R = self.eye[1][1:4] if self.eye[1] else (0.0, 0.0, 0.0)
        self.last = (sample, yaw, pitch, conf, flags)
        if self.shm:
            self.shm.write(sample, yaw, pitch, conf, flags, L, R)
        if self.log is not None:
            self.log.append((now_us, sample, yaw, pitch, conf, flags))


# ------------------------------------------------------------------ BrokenEye client
def read_exact(sock, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError('BrokenEye closed the connection')
        buf += chunk
    return bytes(buf)


NET = {0: {'bytes': 0, 'msgs': 0, 'frames': 0, 'state': 'starting'}, 1: {'bytes': 0, 'msgs': 0, 'frames': 0, 'state': 'starting'}}


def camera_thread(host, port, cam, q, stop):
    request = bytes([0x01 if cam == 0 else 0x02])
    st = NET[cam]
    while not stop.is_set():
        try:
            with socket.create_connection((host, port), timeout=5) as s:
                s.settimeout(5)
                s.sendall(request)
                st['state'] = 'connected, waiting for the first message'
                say(f"[cam {cam}] connected to {host}:{port}, sent request 0x{request[0]:02x}")
                first = True
                while not stop.is_set():
                    try:
                        head = read_exact(s, 5)
                    except socket.timeout:
                        st['state'] = 'connected, no data for 5 s'
                        say(f"[cam {cam}] no data for 5 s (BrokenEye connected? preview or recording on?)")
                        continue
                    st['bytes'] += 5
                    msg_id, length = head[0], struct.unpack('<I', head[1:5])[0]
                    if length > 50_000_000:
                        say(f"[cam {cam}] implausible length {length} (id {msg_id}, header {head.hex(' ')}); reconnecting")
                        break
                    payload = read_exact(s, length)
                    st['bytes'] += length
                    st['msgs'] += 1
                    t = qpc_us()
                    if length < 12:
                        continue
                    w, h, bpp = struct.unpack('<III', payload[:12])
                    if first:
                        say(f"[cam {cam}] first message id={msg_id} len={length} {w}x{h} {bpp} bpp")
                        first = False
                        st['state'] = 'streaming'
                    if bpp != 8 or w * h != length - 12:
                        if st['msgs'] <= 3:
                            say(f"[cam {cam}] unexpected frame layout {w}x{h} {bpp} bpp len {length}; skipping")
                        continue
                    img = np.frombuffer(payload, np.uint8, w * h, 12).reshape(h, w)
                    st['frames'] += 1
                    try:
                        q.put_nowait((cam, img, t))
                    except queue.Full:
                        pass
        except (OSError, ConnectionError) as e:
            st['state'] = f'error: {e}'
            say(f"[cam {cam}] {e}; retrying in 2 s")
            stop.wait(2)


def run_live(args, est):
    q = queue.Queue(maxsize=8)
    stop = threading.Event()
    threads = [threading.Thread(target=camera_thread, args=(args.host, args.port, cam, q, stop), daemon=True) for cam in (0, 1)]
    for t in threads:
        t.start()
    say(f"publishing to shared memory {SHM_NAME}; Ctrl+C to stop")
    say("keys (any window): Ctrl+F8 = save a snapshot of both cameras, Ctrl+F9 = re-centre while looking straight ahead, Ctrl+F10 = reset offsets")
    last_report = time.time()
    key_down = {0x77: False, 0x78: False, 0x79: False}    # VK_F8, VK_F9, VK_F10, each with Ctrl
    get_key = ctypes.windll.user32.GetAsyncKeyState
    next_auto_snap = time.time() + 10.0 if args.snap_every else None
    try:
        while True:
            try:
                cam, img, t = q.get(timeout=0.5)
                est.process(cam, img, t)
            except queue.Empty:
                est.fuse(qpc_us())          # keep the heartbeat and staleness honest
            ctrl = bool(get_key(0x11) & 0x8000)                # VK_CONTROL
            for vk in key_down:
                down = ctrl and bool(get_key(vk) & 0x8000)
                if down and not key_down[vk]:
                    if vk == 0x77:
                        est.snapshot_requested = True
                    elif vk == 0x78:
                        # Manual re-centre replaces the automatic estimate and freezes it until Ctrl+F10.
                        est.drift = {0: np.zeros(2), 1: np.zeros(2)}
                        est.auto = False
                        say("Ctrl+F9 re-centre: " + est.recentre(qpc_us()) + " (automatic correction frozen; Ctrl+F10 resumes it)")
                    else:
                        est.reset_offsets(); say("Ctrl+F10: offsets reset, automatic correction " + ("on" if est.auto else "off"))
                key_down[vk] = down
            if next_auto_snap and time.time() >= next_auto_snap:
                est.snapshot_requested = True
                next_auto_snap = time.time() + args.snap_every
            if time.time() - last_report >= 2.0:
                s = est.stats
                l = est.last
                if s[0][0] == 0 and s[1][0] == 0:
                    say(f"no frames in the last 2 s | cam0: {NET[0]['state']} ({NET[0]['bytes']} B, {NET[0]['msgs']} msgs) "
                        f"| cam1: {NET[1]['state']} ({NET[1]['bytes']} B, {NET[1]['msgs']} msgs)")
                else:
                    say(f"L {s[0][1]}/{s[0][0]}  R {s[1][1]}/{s[1][0]}  gaze ({l[1]:+5.1f},{l[2]:+5.1f}) conf {l[3]:.2f} flags {l[4]} | {est.drift_report()}")
                est.stats = {0: [0, 0], 1: [0, 0]}
                last_report = time.time()
    except KeyboardInterrupt:
        stop.set()


# ------------------------------------------------------------------ replay
def run_replay(args, est):
    root = args.replay
    adv_files = glob.glob(os.path.join(root, 'wearable/advanced/*.json'))
    trk, sysus = [], []
    for p in adv_files:
        d = json.load(open(p))
        trk.append(d['TimestampTrackerUs']); sysus.append(d['TimestampSystemUs'])
    a, b = np.polyfit(trk, sysus, 1)
    frames = []
    for cam in (0, 1):
        for p in glob.glob(os.path.join(root, f'wearable/image/*-{cam}.png')):
            if os.path.getsize(p) > 100:
                t = int(os.path.basename(p).split('-')[0])
                frames.append((int(a * t + b), cam, p))
    frames.sort()
    if args.schedule and not args.realtime:
        # Only the part of the recording the schedule covers matters for scoring.
        import csv as _csv
        qs = [int(r['qpc_us']) for r in _csv.DictReader(l for l in open(args.schedule, encoding='utf-8') if not l.startswith('#'))
              if r['event'] in ('hold_start', 'hold_end')]
        if qs:
            lo, hi = min(qs) - 5_000_000, max(qs) + 5_000_000
            frames = [f for f in frames if lo <= f[0] <= hi]
    print(f"replaying {len(frames)} frames")
    if args.realtime:
        # Pace by the recorded timestamps, stamp frames with the current clock, publish
        # to shared memory, and loop: a stand-in for BrokenEye when testing the layer.
        print("real-time replay into shared memory; Ctrl+C to stop")
        n = 0
        try:
            while True:
                base = qpc_us() - frames[0][0]
                for t_us, cam, p in frames:
                    wait = (t_us + base - qpc_us()) / 1e6
                    if wait > 0:
                        time.sleep(wait)
                    est.process(cam, cv2.imread(p, cv2.IMREAD_GRAYSCALE), qpc_us())
                    n += 1
                    if args.replay_log and n % 300 == 0:
                        l = est.last
                        print(f"gaze ({l[1]:+5.1f},{l[2]:+5.1f}) conf {l[3]:.2f} flags {l[4]}", flush=True)
        except KeyboardInterrupt:
            return
    t0 = time.time()
    for t_us, cam, p in frames:
        est.process(cam, cv2.imread(p, cv2.IMREAD_GRAYSCALE), t_us)
    dt = time.time() - t0
    print(f"processed in {dt:.1f} s ({len(frames) / dt:.0f} frames/s); "
          f"detections L {est.stats[0][1]}/{est.stats[0][0]} R {est.stats[1][1]}/{est.stats[1][0]}")
    log = est.log
    conf = np.array([r[4] for r in log])
    print(f"fused output available (conf > 0) on {100 * (conf > 0).mean():.0f}% of updates; "
          f"both eyes used on {100 * np.mean([r[5] == 3 for r in log]):.0f}%")
    if args.schedule:
        import csv
        rows = list(csv.DictReader(l for l in open(args.schedule, encoding='utf-8') if not l.startswith('#')))
        holds, st = [], None
        for r in rows:
            if r['event'] == 'hold_start':
                st = r
            elif r['event'] == 'hold_end' and st:
                holds.append((int(st['qpc_us']), int(r['qpc_us']), r['label'], int(r['index']), float(r['yaw']), float(r['pitch'])))
                st = None
        times = np.array([r[0] for r in log])
        errs_all = []
        print("hold      target        coverage  median err  (fused output vs target, first 0.4 s of each hold skipped)")
        for s, e, label, idx, yaw, pitch in holds:
            if args.eval_passes and label not in args.eval_passes.split(','):
                continue
            lo, hi = bisect.bisect_left(times, s + 400_000), bisect.bisect_right(times, e)
            seg = log[lo:hi]
            if not seg:
                continue
            ok = [r for r in seg if r[4] > 0]
            errs = []
            for r in ok:
                d1, d2 = angles_to_dir(r[2], r[3]), angles_to_dir(yaw, pitch)
                errs.append(math.degrees(math.acos(max(-1, min(1, sum(x * y for x, y in zip(d1, d2)))))))
            errs_all += errs
            print(f"{label:5s} {idx:2d}  ({yaw:+5.1f},{pitch:+5.1f})   {100 * len(ok) / len(seg):4.0f}%     "
                  f"{np.median(errs) if errs else float('nan'):5.1f}°")
        if errs_all:
            e = np.array(errs_all)
            print(f"overall: median {np.median(e):.1f}°  p90 {np.percentile(e, 90):.1f}°  max {e.max():.1f}°  n={len(e)}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--calib', required=True)
    ap.add_argument('--host', default='127.0.0.1')
    ap.add_argument('--port', type=int, default=5555)
    ap.add_argument('--replay', help='extracted BrokenEye recording dir; runs offline instead of live')
    ap.add_argument('--schedule', help='calib_recorder schedule CSV to score the replay against')
    ap.add_argument('--eval-passes', help='comma-separated hold labels to score, e.g. p2')
    ap.add_argument('--no-shm', action='store_true')
    ap.add_argument('--realtime', action='store_true', help='with --replay: pace by timestamps, publish to shared memory, loop')
    ap.add_argument('--replay-log', action='store_true')
    ap.add_argument('--no-auto', action='store_true', help='disable the automatic slip correction from the shape model')
    ap.add_argument('--auto-tau', type=float, default=5.0, help='time constant in seconds of the automatic slip correction')
    ap.add_argument('--snap-every', type=float, default=0, help='save a snapshot of both cameras every S seconds (0 = only on Ctrl+F8)')
    args = ap.parse_args()
    calib = json.load(open(args.calib, encoding='utf-8'))
    shm = None if (args.no_shm or (args.replay and not args.realtime)) else SharedGaze()
    est = Estimator(calib, shm=shm, log=[] if (args.replay and not args.realtime) else None)
    est.AUTO_TAU_S = args.auto_tau
    est.auto_enabled = not args.no_auto
    est.auto = est.auto_enabled and all(m.shape is not None for m in est.models.values())
    if est.auto_enabled and not est.auto:
        say("calibration has no shape model; automatic slip correction off (re-run calib_fit --write-calib)")
    say(f"calibration: {args.calib} ({calib.get('created')}), eyes: {list(calib['eyes'])}")
    if args.replay:
        run_replay(args, est)
    else:
        run_live(args, est)


if __name__ == '__main__':
    main()
