# Diagnosis: why Tobii fails with the gasket

The first two recordings, made before anything was built. They established that
the cameras see the eye well and that Tobii's rejections are its own.

## Recording 1: `stream-2026-09-27T10-33-37.brokeneye`

15.7 s, gasket fitted, free viewing.

- **Tobii samples:** 1,891 at **121 Hz**.
- **Camera frames:** 472 per camera at **30.2 Hz** (measured). Every interval
  is exactly 4 tracker periods (33.2 ms), no drops, no jitter: a fixed 1-in-4
  decimation at the source (Tobii runtime or BrokenEye), not a slow recorder.
  Confirmed at 30 Hz in every later recording and live.

### Tobii's own data (measured)

| | Left | Right |
|---|---|---|
| `GazeDirectionValidity` true | 6% of samples | 5% |
| `PupilDiameterValidity` true | 6% | 5% |
| `PupilPositionInSensorAreaValidity` true | **0%** (never reported by this tracker) | 0% |
| `PositionGuideValidity` true | 6% | 5% |
| Combined gaze valid | 11% | |
| `ImproveUserPositionHmd` true | 0% | |

- First 8 s: 0% valid on both eyes. Seconds 8–13: 7–23% in 2 s windows, then
  falling away.
- When valid, values were plausible: `PositionGuideXy` ≈ 0.5 (centred),
  `GazeOriginMmXyz` x ≈ +29 / −30 mm (59 mm IPD), y ≈ 0, z −17 to −19 mm;
  pupil 4.8 / 4.2 mm.
- Caveat: the position guide is only reported while tracking is valid, so
  "centred" only describes the moments that worked.

### Camera images (measured, checked by eye)

Contact sheets of the eye images were checked by eye; they are not
published (they show the user's irises).

- **The hardware works.** Pupils are sharp; on tracked frames a ring of about
  eight LED glints sits on the cornea.
- **The eye sits against the nose-side edge of each camera's view,** at a
  steep angle; upper lashes often cross the cornea.
- **Left camera:** a column of three large grainy bright ovals in every frame,
  even with the headset half on: IR LEDs lighting a very close matte surface,
  probably the gasket's nose area (untested). Matte black tape there might cut
  the glare.
- **Right camera:** a dark vertical band with bright dashes along the left
  edge, possibly the gasket too (untested).
- **A simple pupil detector finds a clean pupil in most frames Tobii rejected**
  (`tools/pupil_probe.py`):

| | Tobii valid | Pupil found when Tobii invalid |
|---|---|---|
| Left | 8% of non-blink frames | 82% |
| Right | 6% | 59% |

  A hand check of 12 detections found ~3 false positives (nose shadow, gap
  between glare ovals) and 1 miss at the frame edge. The script's glint counts
  are unreliable (skin speckle); ignore them.
- Strongest single frame: left eye at 13.5 s, frontal, round pupil, full glint
  ring, and Tobii still reports it invalid.

### Interpretation (inferred)

- Not a hardware fault, and not personal calibration (validity is decided
  before it applies).
- Tobii reconstructs a 3D eye position from the glints and discards samples
  outside its designed eye box. The gasket puts the eye outside it.
- The runtime is closed; its thresholds cannot be relaxed from here.

### First accuracy test (`tools/gaze_fit.py`)

Camera frames paired with Tobii-valid samples within 8 ms, pupil centre
regressed onto Tobii's angles, scored on 5-chunk time-blocked cross-validation
(leave-one-out is optimistic because neighbouring frames are near duplicates).

| Eye | Labelled frames | Label span | Predict-the-mean | Quadratic, time-blocked |
|---|---|---|---|---|
| Left | 39 over 6.6 s | yaw −24..+14°, pitch −15..+9° | median 10.7° | median 3.9°, p90 6.1° |
| Right | 22 over 3.4 s | yaw −17..+24°, pitch −10..0° | median 9.9° | median 2.6°, p90 5.3° |

The signal is real (`evidence/pupil-vs-tobii-yaw.png`: yaw smooth and monotonic
in pupil x), but the labels cluster in a few fixations. Verdict: promising.

## Recording 2: `stream-2026-09-27T11-12-36.brokeneye`

59 s during Pimax's own eye-tracking calibration, then free viewing, then the
headset came off.

| | Left | Right |
|---|---|---|
| Tobii gaze valid | 3.2% | 14.9% |
| Labelled frames (Tobii valid within 8 ms, pupil found) | 44 | 260 |
| Quadratic on pupil centre, time-blocked | median 4.8°, p90 24° | median 5.0°, p90 11° |
| Right eye, centre + ellipse shape | | median 3.6°, p90 8.0° |
| Pupil-to-glint vector | worse | worse (~4 of 8 glints on the cornea) |
| Within-fixation detector jitter | 0.71 px ≈ 0.46° | 0.62 px ≈ 0.43° |

- **No-go for self-calibrating from Tobii's valid samples.** They exist for
  only some targets and cluster where Tobii accepts the eye, so the fit
  extrapolates badly. The detector itself is precise (under 0.5° within a
  fixation) and the pupil trace shows clean steps per target.
- **Coverage holes:** for some targets the pupil leaves a camera's frame at the
  nose-side edge. Each eye's blind spot is mostly the other's good view, so a
  two-eye estimator covers most of the field.
- `ImproveUserPositionHmd` was true on every sample here and false on every
  sample of recording 1. Not explained.

This led to the explicit calibration recorder: [calibration.md](calibration.md).

## Tobii's labels are biased anyway (measured in recording 3)

Where Tobii was valid during the calibration session, its yaw matched the
target with the sign flipped (Tobii x points to the user's left) and its
**pitch sat about +8° above the target**, a constant offset. After a per-eye
affine correction its samples agreed with targets across placements only to
3° (right) and 6–9° (left). Do not use Tobii's samples as labels or anchors.
