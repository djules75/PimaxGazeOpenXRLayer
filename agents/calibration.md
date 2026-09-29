# Calibration: recorder, fitting and results

Self-calibration from Tobii's valid samples failed ([diagnosis.md](diagnosis.md)),
so calibration uses explicit targets.

## Procedure

1. **`dist/calib_recorder/calib_recorder.exe`** (`tools/calib_recorder/`): a
   minimal OpenXR + D3D11 app showing a head-locked dot at **15 targets**
   (yaw up to ±30°, pitch up to ±20°), 2 s per hold, two passes, ~85 s. It
   logs the schedule and per-frame runtime gaze on the QPC clock
   (`calib-<stamp>-{schedule,frames}.csv` in the working folder), which
   BrokenEye recordings share. It sets `CHEEKY_OPENXR_LAYER_DISABLE=1` for
   itself and loads `openxr_loader.dll` from its folder. User steps and build:
   `tools/calib_recorder/README.md`.
2. BrokenEye (admin) **records** during the session.
3. Extract the recording to a scratch folder and fit:
   ```
   python tools\calib_fit.py --v2 --write-calib=service\gaze_calib.json <extracted recording> <schedule.csv>
   ```
   It drops holds where the pupil was found on < 40% of frames, and writes the
   per-eye centre map, the shape model and the calibrated pupil range. Without
   `--write-calib` it scores leave-one-target-out and pass 1 → pass 2 per eye,
   and the runtime's gaze against the same targets.
4. Check end to end through the service:
   `python service\gaze_service.py --calib <file> --replay <dir> --schedule <csv> --eval-passes p2`
   with a calibration fitted on pass 1 only.

**Go bar** (set before the first calibration session): pass 1 → pass 2 median
≤ 3° and p90 ≤ 6°.

`calib_recorder --follow` is the live check: faint dots at the 15 targets and a
bright dot at the gaze reported through OpenXR, green when tracked, red when
not. User instructions in `dist/layer/README.md`.

## Sessions and results (all 2026-09-27)

### Recording 3, 14:39: first calibration session, GO

`stream-2026-09-27T14-39-54.brokeneye` (80 s) and
`calib-20260927-143959-*.csv`. 13 targets
(±20° yaw, ±15° pitch). App at 72 Hz worn.

- The first detector missed a third of the targets per camera (slivers, nose
  shadow); `pupil_v2` fixed that.

| `calib_fit.py --v2`, pass 1 → pass 2 | Left | Right |
|---|---|---|
| Targets with pupil found | 13 of 13 | 11 of 13 (misses +20° yaw) |
| Pupil centre | median 3.5°, p90 6.2° | median 1.6°, p90 3.4° |
| Centre + ellipse shape | median 1.8°, p90 5.4°, one 88° outlier | median 1.1°, p90 2.3°, one 25° outlier |
| Leave-one-target-out within a pass | median 3.7°, p90 9.9° | median 2.5°, p90 6.7° |

- Each camera's blind spot is the other's best view: either-eye coverage 26 of
  26 target holds. Through the service with a pass-1 calibration: median 1.1°,
  p90 3.4°.

### Recording 4, 15:11: recalibration and placement study

`stream-2026-09-27T15-11-30.brokeneye` (ran 15 min; only the
schedule window is used) with `calib-20260927-151244-*.csv` (not published).
Pass 1 → pass 2: left 2.3° / 3.2°, right 1.9° / 2.8° (median / p90).

Comparing recordings 3 and 4 (pupil shift ~10 px) gave the placement numbers
and the slip correction in [gaze-service.md](gaze-service.md).

### Recording 5, 16:36: wide grid (current calibration)

`stream-2026-09-27T16-36-20.brokeneye` with
`calib-20260927-163648-*.csv`. 15 targets to ±30° yaw, ±20° pitch, made after
the user saw the dot fall short far left/right.

- At +30° the left eye looks straight into its camera (pupil near x = 40); at
  −30° the left pupil is hidden by lid or nose.
- Pass-1 calibration scored on pass 2 through the service: all 15 targets
  covered 96–100%, **median 1.5° (auto off) / 1.3° (auto on), p90 2.8° / 3.1°**,
  ±30° targets 0.8–3.6°.
- The calibration in use was fitted on both passes of this session.

Recordings, session logs and calibration files are personal and not
published.

## When to recalibrate

When the headset sits differently in a lasting way. Ctrl+F9 and the automatic
correction absorb small shifts; a fresh calibration is still the most accurate
for a given placement.
