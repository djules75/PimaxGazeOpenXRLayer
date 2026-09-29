"""Pupil detector, second attempt.

At this eye position each camera sees the eye from the nose side at a steep
angle. When the eye rotates away from the camera the pupil becomes a thin dark
sliver (aspect ratio up to about 10) that the first detector rejected as "not
compact". The nose shadow and the gaps between the LED glare ovals are the
usual false hits, so detection is restricted to a per-camera region where the
pupil can actually be.

find_pupil_v2(img, cam) -> ((cx, cy), (w, h), angle), fill, score  or None
"""
import cv2
import numpy as np

# x0, x1, y0, y1 in the 200x200 frame. Camera 0 is the left eye, 1 the right.
ROI = {0: (15, 150, 35, 150), 1: (60, 160, 30, 140)}   # cam 0 widened left: at +30° yaw the left pupil sits near x=40


def find_pupil_v2(img, cam):
    g = cv2.GaussianBlur(img, (5, 5), 0)
    x0, x1, y0, y1 = ROI[cam]
    floor = int(np.percentile(g[y0:y1, x0:x1], 1))
    best = None
    for off in (10, 16, 24, 34, 46):
        _, bw = cv2.threshold(g, floor + off, 255, cv2.THRESH_BINARY_INV)
        bw = cv2.morphologyEx(bw, cv2.MORPH_OPEN, np.ones((2, 2), np.uint8))
        cnts, _ = cv2.findContours(bw, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_NONE)
        for c in cnts:
            a = cv2.contourArea(c)
            if a < 30 or a > 3500 or len(c) < 8:
                continue
            (cx, cy), (w, h), ang = el = cv2.fitEllipse(c)
            if not (x0 <= cx <= x1 and y0 <= cy <= y1):
                continue
            if min(w, h) < 2.5 or max(w, h) > 60 or max(w, h) / max(min(w, h), 1) > 12:
                continue
            fill = a / (np.pi * w * h / 4)
            hull = cv2.contourArea(cv2.convexHull(c))
            solidity = a / hull if hull else 0
            if fill < 0.6 or solidity < 0.8:
                continue
            m = np.zeros_like(img)
            cv2.ellipse(m, el, 255, -1)
            ring = cv2.dilate(m, np.ones((7, 7), np.uint8)) & ~m
            if not m.any() or not ring.any():
                continue
            inside, around = g[m > 0].mean(), g[ring > 0].mean()
            contrast = around - inside
            if contrast < 20:
                continue
            score = contrast * np.sqrt(a) * fill * solidity
            if best is None or score > best[2]:
                best = (el, fill, score)
    return best
