"""Accuracy test with labels we control.

Pairs a BrokenEye recording with the schedule written by tools/calib_recorder
(fixation targets at known head-relative angles, timed on the QPC clock that
BrokenEye also records as TimestampSystemUs), detects the pupil in every camera
frame taken while a target was held, and fits pupil position -> gaze angle per
eye.

usage: python calib_fit.py <extracted recording dir> <calib-*-schedule.csv> [calib-*-frames.csv] [out_prefix]

Reports, per eye:
  - coverage: for each target, the share of hold frames with a detected pupil
  - leave-one-target-out accuracy within a pass (field coverage of the model)
  - pass 1 -> pass 2 accuracy (includes drift/slip between passes)
  - the runtime's own eye gaze (from the frames log) against the same targets
Go bar from agents/calibration.md: median <= 3 deg and p90 <= 6 deg.
"""
import bisect, csv, glob, json, math, os, sys
import cv2
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

flags = [a for a in sys.argv[1:] if a.startswith('--')]
args = [a for a in sys.argv[1:] if not a.startswith('--')]
root = args[0]
sched_path = args[1]
frames_path = args[2] if len(args) > 2 and args[2].endswith('.csv') else None
out_prefix = args[3] if len(args) > 3 else (args[2] if len(args) > 2 and not args[2].endswith('.csv') else None)
USE_V2 = '--v2' in flags     # use tools/pupil_v2.py (accepts thin slivers, per-camera region)
# --write-calib=<path>: fit the per-eye maps on the holds and write the service's calibration file.
# --calib-passes=1,2: which passes feed that fit (default: all, including intro/close).
WRITE_CALIB = next((f.split('=', 1)[1] for f in flags if f.startswith('--write-calib=')), None)
CALIB_PASSES = next((set(int(x) for x in f.split('=', 1)[1].split(',')) for f in flags if f.startswith('--calib-passes=')), None)

SETTLE_S = 0.4      # drop the start of each hold: saccade latency and landing
EYE = {0: 'Left', 1: 'Right'}

# --- detector from pupil_probe.py, without running its report
probe = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'pupil_probe.py')
src = open(probe, encoding='utf-8').read()
ns = {}
exec(src.split('frames = {0: [], 1: []}')[0].replace('root = sys.argv[1]', f'root = {root!r}')
     .replace("overlay_out = sys.argv[2] if len(sys.argv) > 2 else None", 'overlay_out = None'), ns)
find_pupil, adv, ts = ns['find_pupil'], ns['adv'], ns['ts']
if USE_V2:
    from pupil_v2 import find_pupil_v2
    detect = find_pupil_v2
    print("detector: pupil_v2")
else:
    detect = lambda img, cam: find_pupil(img)
    print("detector: pupil_probe (v1)")

# --- tracker clock -> system (QPC) clock, from the advanced samples
trk = np.array(ts, float)
sysus = np.array([adv[t]['TimestampSystemUs'] for t in ts], float)
a, b = np.polyfit(trk, sysus, 1)
resid = sysus - (a * trk + b)
print(f"clock map: system = {a:.6f} * tracker + {b:.0f}; residual p95 {np.percentile(np.abs(resid), 95):.0f} us")
def to_sys(t): return a * t + b

# --- schedule
holds = []          # (start_us, end_us, label, pass, index, yaw, pitch, dir xyz)
meta = {}
with open(sched_path, encoding='utf-8') as f:
    for line in f:
        if line.startswith('#'):
            for kv in line[1:].split():
                if '=' in kv:
                    k, v = kv.split('=', 1); meta[k] = v
            continue
        break
rows = list(csv.DictReader(l for l in open(sched_path, encoding='utf-8') if not l.startswith('#')))
start = None
for r in rows:
    if r['event'] == 'hold_start':
        start = r
    elif r['event'] == 'hold_end' and start is not None:
        holds.append((int(start['qpc_us']), int(r['qpc_us']), r['label'], int(r['pass']), int(r['index']),
                      float(r['yaw']), float(r['pitch']), np.array([float(r['x']), float(r['y']), float(r['z'])])))
        start = None
distance = float(meta.get('distance_m', 1.5))
print(f"schedule: {len(holds)} holds, passes {sorted(set(h[3] for h in holds))}, distance {distance} m")
if not holds:
    sys.exit("no holds in the schedule")
rec_lo, rec_hi = sysus.min(), sysus.max()
inside = sum(1 for h in holds if rec_lo <= h[0] and h[1] <= rec_hi)
print(f"holds inside the recording's time span: {inside}/{len(holds)}")

# --- eye positions in the head frame (from the frames log), else a nominal IPD
eye_pos = {0: np.array([-0.0315, 0, 0]), 1: np.array([0.0315, 0, 0])}
gaze_rows = []
if frames_path:
    fr = list(csv.DictReader(l for l in open(frames_path, encoding='utf-8') if not l.startswith('#')))
    L = np.array([[float(r['eyeL_x']), float(r['eyeL_y']), float(r['eyeL_z'])] for r in fr])
    R = np.array([[float(r['eyeR_x']), float(r['eyeR_y']), float(r['eyeR_z'])] for r in fr])
    eye_pos = {0: np.median(L, 0), 1: np.median(R, 0)}
    print(f"eye positions (head frame, m): L {eye_pos[0].round(4)} R {eye_pos[1].round(4)}")
    gaze_rows = fr


def angles(d):
    x, y, z = d
    return math.degrees(math.atan2(x, -z)), math.degrees(math.atan2(y, math.hypot(x, z)))


def vec(yaw, pitch):
    y, p = math.radians(yaw), math.radians(pitch)
    return np.array([math.sin(y) * math.cos(p), math.sin(p), -math.cos(y) * math.cos(p)])


def ang_err(A, B):
    return np.array([math.degrees(math.acos(np.clip(vec(*u) @ vec(*v), -1, 1))) for u, v in zip(A, B)])


def summ(e):
    return f"median {np.median(e):4.1f}°  p90 {np.percentile(e, 90):4.1f}°  max {e.max():4.1f}°  n={len(e)}"


def design(F, order):
    cols = [np.ones(len(F))] + [F[:, i] for i in range(F.shape[1])]
    if order >= 2:
        for i in range(F.shape[1]):
            for j in range(i, F.shape[1]):
                cols.append(F[:, i] * F[:, j])
    return np.stack(cols, 1)


def fit(Ftr, Atr, order, lam=1e-2):
    mu, sd = Ftr.mean(0), Ftr.std(0) + 1e-9
    X = design((Ftr - mu) / sd, order)
    W = np.linalg.solve(X.T @ X + lam * np.eye(X.shape[1]), X.T @ Atr)
    return lambda F: design((F - mu) / sd, order) @ W


# --- per-eye target angles for each hold, and labelled frames
results = {}
all_coverage = {}
calib_data = {}
calib_shape = {}
for cam in (0, 1):
    eye = EYE[cam]
    frames = sorted(int(os.path.basename(p).split('-')[0]) for p in glob.glob(os.path.join(root, f'wearable/image/*-{cam}.png'))
                    if os.path.getsize(p) > 100)
    fsys = np.array([to_sys(t) for t in frames])
    data = []      # (hold idx, feature vec, per-eye target angles)
    coverage = []
    for hi, (s, e, label, pas, idx, yaw, pitch, d) in enumerate(holds):
        tgt = d * distance
        tyaw, tpitch = angles(tgt - eye_pos[cam])
        lo, hi_ = bisect.bisect_left(fsys, s + SETTLE_S * 1e6), bisect.bisect_right(fsys, e)
        found = 0
        for k in range(lo, hi_):
            img = cv2.imread(os.path.join(root, f'wearable/image/{frames[k]}-{cam}.png'), cv2.IMREAD_GRAYSCALE)
            p = detect(img, cam)
            if p is None:
                continue
            (cx, cy), (w, h), ang = p[0]
            data.append((hi, np.array([cx, cy, w, h, math.cos(2 * math.radians(ang)), math.sin(2 * math.radians(ang))]),
                         np.array([tyaw, tpitch]), np.array([yaw, pitch]), p[2]))
            found += 1
        coverage.append((label, idx, yaw, pitch, hi_ - lo, found))
    all_coverage[cam] = {(c[0], c[1]): (c[4], c[5]) for c in coverage}
    if not data:
        print(f"\n== {eye}: no labelled frames"); continue
    H = np.array([d[0] for d in data]); F = np.array([d[1] for d in data]); A = np.array([d[2] for d in data])
    # Outliers: within a hold the pupil should not move, so drop frames far from
    # the hold's median (lash/shadow false hits), then drop whole holds whose
    # median sits far from the others (glare-gap false clusters).
    keep = np.ones(len(F), bool)
    hold_med = {}
    for hidx in set(H):
        m = H == hidx
        med = np.median(F[m, :2], 0)
        keep[m] &= np.hypot(*(F[m, :2] - med).T) < 10
        hold_med[hidx] = np.median(F[m & keep, :2], 0) if (m & keep).any() else med
    centre = np.median(np.array(list(hold_med.values())), 0)
    bad_holds = {h for h, mv in hold_med.items() if np.hypot(*(mv - centre)) > 70}
    # A hold where the pupil was found on under 40% of frames is one where the pupil
    # was hidden (lid, nose) and the few detections are lashes or shadows.
    bad_holds |= {h for h in set(H) if coverage[h][4] and coverage[h][5] / coverage[h][4] < 0.4}
    for hidx in bad_holds:
        keep[H == hidx] = False
    n_frames_dropped = int((~keep).sum())
    Acomb = np.array([d[3] for d in data])[keep]      # target angle from the head centre (what the service predicts)
    score = np.array([d[4] for d in data])[keep]
    H, F, A = H[keep], F[keep], A[keep]
    calib_data[cam] = (F[:, :2].copy(), Acomb, H.copy(), score)
    ratio = np.minimum(F[:, 2], F[:, 3]) / np.maximum(np.maximum(F[:, 2], F[:, 3]), 1e-6)
    calib_shape[cam] = np.c_[F[:, 2:6], ratio]
    print(f"\n== {eye}: {len(F)} labelled frames over {len(set(H))} holds "
          f"(dropped {n_frames_dropped} frames, {len(bad_holds)} whole holds as false clusters)")
    print("   coverage per target (label idx yaw pitch: frames in hold, pupil found)")
    for label, idx, yaw, pitch, n, found in coverage:
        flag = '' if n == 0 or found / n >= 0.5 else '   <-- weak'
        print(f"     {label:5s} {idx:2d} ({yaw:+5.1f},{pitch:+5.1f}): {n:3d} frames, {found:3d} found{flag}")

    passes = sorted(p for p in set(holds[h][3] for h in H) if p > 0)   # pass 0 = intro/close, centre only
    for name, cols, order in (('pupil centre, quadratic', slice(0, 2), 2), ('centre + ellipse shape, quadratic', slice(0, 6), 2)):
        Fs = F[:, cols]
        # (a) leave-one-target-out within each pass
        errs = []
        for p in passes:
            inp = np.array([holds[h][3] == p for h in H])
            for hidx in sorted(set(H[inp])):
                te = inp & (H == hidx); tr = inp & (H != hidx)
                if tr.sum() < 12 or te.sum() == 0:
                    continue
                pred = fit(Fs[tr], A[tr], order)(Fs[te])
                errs.append(ang_err(pred, A[te]))
        loto = np.concatenate(errs) if errs else np.array([np.nan])
        line = f"   {name:36s} leave-one-target-out {summ(loto)}"
        # (b) pass 1 -> pass 2
        if len(passes) >= 2:
            tr = np.array([holds[h][3] == passes[0] for h in H]); te = np.array([holds[h][3] == passes[1] for h in H])
            if tr.sum() >= 12 and te.sum() > 0:
                pred = fit(Fs[tr], A[tr], order)(Fs[te])
                e12 = ang_err(pred, A[te])
                line += f" | pass1->pass2 {summ(e12)}"
                results[(cam, name)] = (Fs, A, H, e12)
        print(line)
    if len(passes) >= 2:
        # per-target error for the pass1->pass2 pupil-centre model
        Fs = F[:, :2]; tr = np.array([holds[h][3] == passes[0] for h in H]); te = np.array([holds[h][3] == passes[1] for h in H])
        if tr.sum() >= 12 and te.sum() > 0:
            model = fit(Fs[tr], A[tr], 2)
            print("   pass1->pass2 per target (idx yaw pitch: median error)")
            for hidx in sorted(set(H[te])):
                m = te & (H == hidx)
                e = ang_err(model(Fs[m]), A[m])
                print(f"     {holds[hidx][4]:2d} ({holds[hidx][5]:+5.1f},{holds[hidx][6]:+5.1f}): {np.median(e):4.1f}°  n={m.sum()}")

# --- either-eye coverage: a two-eye estimator uses whichever camera sees the pupil
if 0 in all_coverage and 1 in all_coverage:
    keys = sorted(set(all_coverage[0]) | set(all_coverage[1]), key=lambda k: (k[0], k[1]))
    ok = 0; total = 0
    weak = []
    for k in keys:
        if k[0] in ('intro', 'close'):
            continue
        total += 1
        fr = [all_coverage[c].get(k, (0, 0)) for c in (0, 1)]
        best = max((f[1] / f[0] if f[0] else 0) for f in fr)
        if best >= 0.5:
            ok += 1
        else:
            weak.append(k)
    print(f"\n== either-eye coverage: {ok}/{total} targets have >= 50% of hold frames with a pupil in at least one camera"
          + (f"; weak: {weak}" if weak else ""))

# --- Tobii's own samples from the recording against the same targets.
# Tobii's HMD frame has x pointing to the user's left (the left eye origin has
# positive x), so its yaw is negated to match the app's convention.
print("\n== Tobii samples in the recording during holds (validity and accuracy per target)")
sys_sorted = np.array(sorted(sysus))
order = np.argsort(sysus)
ts_by_sys = [ts[i] for i in order]
print("   idx (yaw,pitch)   L valid%  L median err   R valid%  R median err")
tob_err = {0: [], 1: []}
for hi, (s, e, label, pas, idx, yaw, pitch, d) in enumerate(holds):
    lo, hi_ = bisect.bisect_left(sys_sorted, s + SETTLE_S * 1e6), bisect.bisect_right(sys_sorted, e)
    samples = [adv[ts_by_sys[k]] for k in range(lo, hi_)]
    if not samples:
        continue
    cells = []
    for cam in (0, 1):
        eye = EYE[cam]
        tgt = d * distance
        ty, tp = angles(tgt - eye_pos[cam])
        v = [sm[eye]['GazeDirectionNormalizedXyz'] for sm in samples if sm[eye]['GazeDirectionValidity']]
        if v:
            ang = [(-math.degrees(math.atan2(g[0], g[2])), math.degrees(math.atan2(g[1], math.hypot(g[0], g[2])))) for g in v]
            err = ang_err(ang, [[ty, tp]] * len(ang))
            tob_err[cam].extend(err)
            cells.append(f"{100 * len(v) / len(samples):5.0f}%   {np.median(err):5.1f}°     ")
        else:
            cells.append(f"{0:5.0f}%       -       ")
    print(f"   {label:5s} {idx:2d} ({yaw:+5.1f},{pitch:+5.1f})  " + ''.join(cells))
for cam in (0, 1):
    if tob_err[cam]:
        print(f"   {EYE[cam]} Tobii accuracy when valid: {summ(np.array(tob_err[cam]))}")

# --- the runtime's own gaze against the same targets
if gaze_rows:
    print("\n== runtime eye gaze (XR_EXT_eye_gaze_interaction) during holds")
    errs = []; fresh_errs = []; valid = 0; total = 0; fresh = 0
    per = {}
    prev = None
    for r in gaze_rows:
        cur = (float(r['gaze_yaw']), float(r['gaze_pitch']))
        changed = prev is not None and (abs(cur[0] - prev[0]) > 1e-3 or abs(cur[1] - prev[1]) > 1e-3)
        prev = cur
        if r['phase'] != 'hold':
            continue
        total += 1
        flags = int(r['gaze_flags'])
        if r['gaze_active'] == '1' and flags & 0x1 and flags & 0x4:   # orientation valid and tracked
            valid += 1
            # runtime gaze is the combined gaze from between the eyes; compare with the combined target angle
            e = ang_err([cur], [[float(r['tgt_yaw']), float(r['tgt_pitch'])]])[0]
            errs.append(e); per.setdefault(int(r['index']), []).append(e)
            if changed:
                fresh += 1; fresh_errs.append(e)
    print(f"   hold frames {total}, gaze valid+tracked {valid} ({100 * valid / max(total, 1):.0f}%), "
          f"value changed since previous frame on {fresh} ({100 * fresh / max(total, 1):.0f}%)")
    if errs:
        print(f"   accuracy when flagged valid: {summ(np.array(errs))}")
    if fresh_errs:
        print(f"   accuracy on frames where the value changed (fresh): {summ(np.array(fresh_errs))}")
    for idx in sorted(per):
        print(f"     target {idx:2d}: flagged valid {len(per[idx]):4d} frames, median {np.median(per[idx]):4.1f}°")

# --- calibration file for the gaze service
if WRITE_CALIB:
    import datetime
    calib = {
        'version': 1,
        'created': datetime.datetime.now().isoformat(timespec='seconds'),
        'source': {'recording': os.path.abspath(root), 'schedule': os.path.abspath(sched_path),
                   'detector': 'pupil_v2' if USE_V2 else 'pupil_probe', 'passes': sorted(CALIB_PASSES) if CALIB_PASSES else 'all'},
        'angles': 'yaw + right, pitch + up, degrees, head frame, from the head centre',
        'design': '1,x,y,xx,xy,yy on standardised (cx,cy)',
        'eyes': {},
    }
    for cam in (0, 1):
        if cam not in calib_data:
            continue
        F, Acomb, H, score = calib_data[cam]
        sel = np.ones(len(F), bool) if CALIB_PASSES is None else np.array([holds[h][3] in CALIB_PASSES for h in H])
        if sel.sum() < 12:
            print(f"   {EYE[cam]}: too few frames for a calibration ({sel.sum()})"); continue
        Ftr, Atr = F[sel], Acomb[sel]
        mu, sd = Ftr.mean(0), Ftr.std(0) + 1e-9
        X = design((Ftr - mu) / sd, 2)
        W = np.linalg.solve(X.T @ X + 1e-2 * np.eye(X.shape[1]), X.T @ Atr)
        res = ang_err(X @ W, Atr)
        lo, hi_ = Ftr.min(0), Ftr.max(0)
        # Shape model: ellipse w, h, cos/sin of twice the angle, and axis ratio. It ignores
        # where the pupil sits in the image, so it survives a headset slip; the service
        # uses it to correct the centre model's offset slowly.
        Fs_all = calib_shape[cam][sel]
        mu_s, sd_s = Fs_all.mean(0), Fs_all.std(0) + 1e-9
        Xs = design((Fs_all - mu_s) / sd_s, 2)
        Ws = np.linalg.solve(Xs.T @ Xs + 1e-1 * np.eye(Xs.shape[1]), Xs.T @ Atr)
        res_s = ang_err(Xs @ Ws, Atr)
        calib['eyes'][EYE[cam].lower()] = {
            'camera': cam, 'mu': mu.tolist(), 'sd': sd.tolist(), 'W': W.tolist(),
            'pupil_bounds': {'x': [float(lo[0]) - 8, float(hi_[0]) + 8], 'y': [float(lo[1]) - 8, float(hi_[1]) + 8]},
            'pupil_range': {'x': [float(lo[0]), float(hi_[0])], 'y': [float(lo[1]), float(hi_[1])]},
            'score_median': float(np.median(score[sel])),
            'fit_residual_deg': {'median': float(np.median(res)), 'p90': float(np.percentile(res, 90))},
            'shape': {'features': 'w,h,cos2a,sin2a,min/max', 'mu': mu_s.tolist(), 'sd': sd_s.tolist(), 'W': Ws.tolist(),
                      'fit_residual_deg': {'median': float(np.median(res_s)), 'p90': float(np.percentile(res_s, 90))}},
            'frames': int(sel.sum()), 'targets': int(len(set(H[sel]))),
        }
        print(f"   calibration {EYE[cam]}: {int(sel.sum())} frames, {len(set(H[sel]))} holds, centre fit residual {summ(res)}; shape fit residual {summ(res_s)}")
    with open(WRITE_CALIB, 'w', encoding='utf-8') as f:
        json.dump(calib, f, indent=1)
    print(f"wrote {WRITE_CALIB}")

if out_prefix and results:
    tiles = []
    for cam in (0, 1):
        key = (cam, 'pupil centre, quadratic')
        if key not in results:
            continue
        Fs, A, H, e = results[key]
        img = np.zeros((400, 400, 3), np.uint8)
        for (x, y), (yaw, pitch) in zip(Fs, A):
            c = int(np.clip((yaw + 30) / 60 * 255, 0, 255))
            cv2.circle(img, (int(x * 2), int(y * 2)), 3, cv2.applyColorMap(np.uint8([[c]]), cv2.COLORMAP_JET)[0, 0].tolist(), -1)
        cv2.putText(img, f"{EYE[cam]}: pupil xy, colour = target yaw", (5, 15), cv2.FONT_HERSHEY_SIMPLEX, 0.45, (255, 255, 255), 1)
        tiles.append(img)
    if tiles:
        cv2.imwrite(out_prefix + '-scatter.png', np.hstack(tiles))
        print(f"wrote {out_prefix}-scatter.png")
