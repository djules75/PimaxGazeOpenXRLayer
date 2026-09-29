"""Find the pupil and corneal glints in a BrokenEye recording's camera frames
and compare against Tobii's own validity for the same moment.

usage: python pupil_probe.py <extracted recording dir> [overlay_out.png]
"""
import bisect, glob, json, os, sys
import cv2
import numpy as np

root = sys.argv[1]
overlay_out = sys.argv[2] if len(sys.argv) > 2 else None

adv = {int(os.path.basename(p)[:-5]): json.load(open(p))
       for p in glob.glob(os.path.join(root, 'wearable/advanced/*.json'))}
ts = sorted(adv)
t0 = ts[0]
EYE = {0: 'Left', 1: 'Right'}   # camera 0 shows the left eye (full glint ring on L-valid frames)


def nearest(t):
    i = bisect.bisect_left(ts, t)
    return min(ts[max(0, i - 1):i + 1], key=lambda x: abs(x - t))


def find_pupil(img):
    """Darkest compact ellipse that is not the nose shadow. Returns (ellipse, fill) or None."""
    g = cv2.GaussianBlur(img, (5, 5), 0)
    best = None
    # The pupil is the darkest compact thing; sweep a few thresholds above the floor.
    floor = int(np.percentile(g, 1))
    for off in (12, 20, 30, 42):
        _, bw = cv2.threshold(g, floor + off, 255, cv2.THRESH_BINARY_INV)
        bw = cv2.morphologyEx(bw, cv2.MORPH_OPEN, np.ones((3, 3), np.uint8))
        cnts, _ = cv2.findContours(bw, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_NONE)
        for c in cnts:
            a = cv2.contourArea(c)
            if not (80 <= a <= 3500) or len(c) < 12:
                continue
            (cx, cy), (w, h), ang = el = cv2.fitEllipse(c)
            if min(w, h) < 7 or max(w, h) / max(min(w, h), 1) > 2.6:
                continue
            fill = a / (np.pi * w * h / 4)
            hull = cv2.contourArea(cv2.convexHull(c))
            solidity = a / hull if hull else 0
            if fill < 0.72 or solidity < 0.85:
                continue
            # Interior must be genuinely dark relative to its surround.
            m = np.zeros_like(img)
            cv2.ellipse(m, el, 255, -1)
            ring = cv2.dilate(m, np.ones((9, 9), np.uint8)) & ~m
            inside, around = g[m > 0].mean(), g[ring > 0].mean()
            if around - inside < 25:
                continue
            score = fill * solidity * (around - inside)
            if best is None or score > best[2]:
                best = (el, fill, score)
    return best


def count_glints(img, el):
    (cx, cy), (w, h), _ = el
    r = max(w, h) / 2
    _, bw = cv2.threshold(img, 235, 255, cv2.THRESH_BINARY)
    n, _, stats, cent = cv2.connectedComponentsWithStats(bw)
    k = 0
    for i in range(1, n):
        if stats[i, cv2.CC_STAT_AREA] > 30:
            continue                               # skin glare, not a corneal reflection
        dx, dy = cent[i][0] - cx, cent[i][1] - cy
        if (dx * dx + dy * dy) ** 0.5 <= r * 2.2:  # the LED ring images on the cornea around the pupil
            k += 1
    return k


frames = {0: [], 1: []}
for p in glob.glob(os.path.join(root, 'wearable/image/*.png')):
    t, c = os.path.basename(p)[:-4].split('-')
    if os.path.getsize(p) > 100:
        frames[int(c)].append(int(t))

rows = []
for cam, tl in frames.items():
    for t in sorted(tl):
        img = cv2.imread(os.path.join(root, f'wearable/image/{t}-{cam}.png'), cv2.IMREAD_GRAYSCALE)
        a = adv[nearest(t)][EYE[cam]]
        p = find_pupil(img)
        rows.append(dict(t=t, cam=cam, tobii=a['GazeDirectionValidity'], blink=a['Blink'],
                         pupil=p is not None, glints=count_glints(img, p[0]) if p else 0,
                         el=p[0] if p else None))

print(f"duration {(ts[-1] - t0) / 1e6:.1f}s")
for cam in (0, 1):
    R = [r for r in rows if r['cam'] == cam and not r['blink']]
    n = len(R)
    tv = [r for r in R if r['tobii']]
    ti = [r for r in R if not r['tobii']]
    pf = lambda L: 100 * sum(r['pupil'] for r in L) / len(L) if L else 0
    print(f"{EYE[cam]:5s} frames(no blink) {n:4d} | Tobii valid {100 * len(tv) / n:4.0f}% "
          f"| pupil found {pf(R):4.0f}% overall, {pf(tv):4.0f}% when Tobii valid, {pf(ti):4.0f}% when Tobii invalid")
    for label, L in (('Tobii valid', tv), ('Tobii invalid', ti)):
        g = [r['glints'] for r in L if r['pupil']]
        if g:
            hist = {k: sum(1 for x in g if x == k) for k in range(0, 10)}
            print(f"      glints near pupil ({label:13s}) median {sorted(g)[len(g) // 2]}  hist {hist}")
    # pupil centre spread: where in the 200x200 frame the eye sits
    xs = [r['el'][0][0] for r in R if r['pupil']]
    ys = [r['el'][0][1] for r in R if r['pupil']]
    if xs:
        print(f"      pupil centre x {min(xs):.0f}..{max(xs):.0f} (median {sorted(xs)[len(xs) // 2]:.0f}), "
              f"y {min(ys):.0f}..{max(ys):.0f} (median {sorted(ys)[len(ys) // 2]:.0f}) of 200")

if overlay_out:
    # A sample of Tobii-invalid frames with the detection drawn, to check the detector by eye.
    pick = [r for r in rows if not r['tobii'] and not r['blink']]
    pick = [pick[int(i * (len(pick) - 1) / 11)] for i in range(12)]
    tiles = []
    for r in pick:
        img = cv2.imread(os.path.join(root, f"wearable/image/{r['t']}-{r['cam']}.png"))
        if r['el']:
            cv2.ellipse(img, r['el'], (0, 255, 0), 1)
        cv2.putText(img, f"{(r['t'] - t0) / 1e6:.1f}s {EYE[r['cam']][0]} g{r['glints']}", (3, 12),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.4, (0, 255, 255), 1)
        tiles.append(img)
    grid = np.vstack([np.hstack(tiles[i:i + 4]) for i in range(0, 12, 4)])
    cv2.imwrite(overlay_out, cv2.resize(grid, None, fx=2, fy=2))
