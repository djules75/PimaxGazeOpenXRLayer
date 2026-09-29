"""Go/no-go accuracy test: can pupil position in the raw camera frame predict
Tobii's gaze direction, using Tobii's own sporadic valid samples as labels?

usage: python gaze_fit.py <extracted recording dir> [scatter_out.png]
"""
import bisect, glob, json, math, os, sys
import cv2
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from importlib import util
spec = util.spec_from_file_location('probe', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'pupil_probe.py'))

root = sys.argv[1]
scatter_out = sys.argv[2] if len(sys.argv) > 2 else None

# Reuse the detector without running the probe's report.
src = open(spec.origin, encoding='utf-8').read()
ns = {}
exec(src.split('frames = {0: [], 1: []}')[0].replace('root = sys.argv[1]', f'root = {root!r}')
     .replace("overlay_out = sys.argv[2] if len(sys.argv) > 2 else None", 'overlay_out = None'), ns)
find_pupil = ns['find_pupil']
adv, ts = ns['adv'], ns['ts']

EYE = {0: 'Left', 1: 'Right'}
MAX_DT = 8300  # one tracker period: gaze barely moves in 8 ms outside a saccade


def angles(d):
    x, y, z = d
    return math.degrees(math.atan2(x, z)), math.degrees(math.atan2(y, math.hypot(x, z)))


def vec(yaw, pitch):
    y, p = math.radians(yaw), math.radians(pitch)
    return np.array([math.sin(y) * math.cos(p), math.sin(p), math.cos(y) * math.cos(p)])


def design(P, order):
    x, y = P[:, 0] / 100 - 1, P[:, 1] / 100 - 1
    cols = [np.ones_like(x), x, y]
    if order >= 2:
        cols += [x * x, y * y, x * y]
    return np.stack(cols, 1)


def fit_predict(Ptr, Atr, Pte, order):
    X = design(Ptr, order)
    lam = 1e-3 * np.eye(X.shape[1])   # tiny ridge so small folds stay well conditioned
    W = np.linalg.solve(X.T @ X + lam, X.T @ Atr)
    return design(Pte, order) @ W


def ang_err(A, B):
    return np.array([math.degrees(math.acos(np.clip(vec(*a) @ vec(*b), -1, 1))) for a, b in zip(A, B)])


def summary(e):
    return f"median {np.median(e):4.1f}°  p90 {np.percentile(e, 90):4.1f}°  max {e.max():4.1f}°"


results = {}
for cam in (0, 1):
    eye = EYE[cam]
    frames = sorted(int(os.path.basename(p).split('-')[0]) for p in glob.glob(os.path.join(root, f'wearable/image/*-{cam}.png'))
                    if os.path.getsize(p) > 100)
    rows = []
    for t in frames:
        i = bisect.bisect_left(ts, t)
        cands = [s for s in ts[max(0, i - 2):i + 2] if abs(s - t) <= MAX_DT and adv[s][eye]['GazeDirectionValidity']]
        if not cands:
            continue
        s = min(cands, key=lambda s: abs(s - t))
        img = cv2.imread(os.path.join(root, f'wearable/image/{t}-{cam}.png'), cv2.IMREAD_GRAYSCALE)
        p = find_pupil(img)
        if p is None:
            continue
        rows.append((t, p[0][0], angles(adv[s][eye]['GazeDirectionNormalizedXyz'])))
    rows.sort()
    med = np.median([r[1] for r in rows], 0)
    rows = [r for r in rows if np.hypot(*(np.array(r[1]) - med)) < 60]  # drop detector outliers far from the pupil cluster
    T = np.array([r[0] for r in rows])
    P = np.array([r[1] for r in rows])
    A = np.array([r[2] for r in rows])
    n = len(rows)
    print(f"\n{eye}: {n} labelled frames (Tobii valid within 8 ms and pupil detected), "
          f"spanning {(T[-1] - T[0]) / 1e6:.1f}s; gaze yaw {A[:, 0].min():+.0f}..{A[:, 0].max():+.0f}°, "
          f"pitch {A[:, 1].min():+.0f}..{A[:, 1].max():+.0f}°")
    base = ang_err(np.repeat(A.mean(0, keepdims=True), n, 0), A)
    print(f"  baseline (always predict mean gaze)  {summary(base)}")
    for order in (1, 2):
        k = design(P[:1], order).shape[1]
        # leave-one-out: optimistic, neighbours in time are near-duplicates
        loo = np.array([fit_predict(np.delete(P, i, 0), np.delete(A, i, 0), P[i:i + 1], order)[0] for i in range(n)])
        e_loo = ang_err(loo, A)
        # blocked: 5 contiguous time chunks, each predicted from the other four - the honest number
        folds = np.array_split(np.arange(n), 5)
        blk = np.zeros_like(A)
        for f in folds:
            tr = np.setdiff1d(np.arange(n), f)
            blk[f] = fit_predict(P[tr], A[tr], P[f], order)
        e_blk = ang_err(blk, A)
        name = 'linear   ' if order == 1 else 'quadratic'
        print(f"  {name} ({k} params/axis)  leave-one-out {summary(e_loo)} | time-blocked {summary(e_blk)}")
        results[(eye, order)] = (P, A, blk, e_blk)

if scatter_out:
    # Pupil positions coloured by Tobii yaw: a smooth gradient means pupil position carries the gaze signal.
    tiles = []
    for cam in (0, 1):
        P, A, _, _ = results[(EYE[cam], 1)]
        img = np.zeros((400, 400, 3), np.uint8)
        for (x, y), (yaw, pitch) in zip(P, A):
            c = int(np.clip((yaw + 30) / 60 * 255, 0, 255))
            cv2.circle(img, (int(x * 2), int(y * 2)), 4, cv2.applyColorMap(np.uint8([[c]]), cv2.COLORMAP_JET)[0, 0].tolist(), -1)
        cv2.putText(img, f"{EYE[cam]}: pupil xy, colour=Tobii yaw", (5, 15), cv2.FONT_HERSHEY_SIMPLEX, 0.45, (255, 255, 255), 1)
        tiles.append(img)
    cv2.imwrite(scatter_out, np.hstack(tiles))
