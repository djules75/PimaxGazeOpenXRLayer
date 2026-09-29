# The gaze service (`service/gaze_service.py`)

## Pipeline

1. **Frames:** one BrokenEye RAW TCP connection per camera (request bytes
   0x01 / 0x02), payloads read to completion, stamped with QPC on arrival
   ([brokeneye.md](brokeneye.md)). Camera 0 is the left eye.
2. **Pupil:** `tools/pupil_v2.py` per frame. It accepts thin slivers (axis
   ratio up to ~10, the pupil seen edge-on when the eye turns away from that
   camera) inside a per-camera region; camera 0's region starts at x = 15
   because at +30° yaw the left pupil sits near x = 40. It rejects the nose
   shadow and glare ovals by position.
3. **Per-eye map:** quadratic from pupil centre to yaw/pitch, from
   `service/gaze_calib.json` ([calibration.md](calibration.md)). Beyond the
   calibrated pupil range it continues in a straight line, and confidence
   fades with distance outside it instead of dropping.
4. **Fusion:** confidence-weighted mean of the eyes that are fresh (< 80 ms)
   and confident (≥ 0.5), each also weighted by its **fold weight** (yaw
   sensitivity along the centre direction relative to its centre value). Each
   eye's quadratic folds over on the side where that eye turns away from its
   camera (left near +22°, right near −24°); without the fold weight, averaging
   pulled far-left/right gaze in to about ±17°.
5. **Publish** to shared memory `Local\PimaxGazeLayer`, with a heartbeat even
   when no frames arrive.

## Placement sensitivity and slip correction

The pupil-centre map is exact for the placement it was calibrated in; a
shift of ~10 px (a few mm) costs 10–26° median. Two corrections:

- **Automatic (default on):** the calibration also holds a shape-only map (ellipse
  w, h, cos 2θ, sin 2θ, axis ratio) that barely depends on where the pupil sits
  (3–4° across placements, but 1–1.7° per-frame jitter). The service adds a
  per-eye EMA (τ = 5 s, clamp 15°, outlier gate 12°) of shape-gaze minus
  centre-gaze to the centre map's output, learned only where fold > 0.8.
  Across the two placements recorded on 2026-09-27: 9.6° → 3.8° median
  (p90 12.2° → 7.0°); within one placement neutral (1.2° → 1.0°).
  `--auto-tau S`, `--no-auto`.
- **Manual (Ctrl+F9):** while the user looks straight ahead, solves (Newton) a
  per-eye pupil-space shift so the current pupil maps to 0°, and freezes the
  automatic correction. **Ctrl+F10** resets both and resumes automatic.
- **Ctrl+F8** saves both cameras with the detected pupil drawn to
  `%LOCALAPPDATA%\PimaxGazeLayer\snap-<time>-cam{0,1}.png`
  (`--snap-every S` for periodic).
- Keys are polled with `GetAsyncKeyState` (focus not needed, key not
  swallowed; games still see it). Polling is per frame, or every 0.5 s with no
  frames.

## Running

- **`PimaxGaze.exe`** (`tools/launcher/`, shipped in `dist/layer/`): registers
  the layer from its own folder (removing PimaxGaze registrations in other
  folders), finds the repo by walking up to `service\gaze_service.py`, runs
  the service with `--calib service\gaze_calib.json` unless given, forwards
  other options, and keeps the service in a job object so it dies with the
  launcher. Refuses to start a second service while one's heartbeat is fresh.
  Launcher options: `--python`, `--repo`, `--manifest`, `--no-register`,
  `--register-only`, `--unregister`. It never starts BrokenEye.
- By hand: `python service\gaze_service.py --calib service\gaze_calib.json`.
- Every 2 s it prints detections per eye and the fused gaze, or bytes per
  camera when no frames arrive. Log: `%LOCALAPPDATA%\PimaxGazeLayer\service.log`.

## Replay (offline testing)

- `--replay <extracted recording> --schedule <calib-*-schedule.csv>` runs the
  whole pipeline on a recording and scores it against the targets
  (`--eval-passes p2` to score one pass). Only the schedule window is used.
- `--replay ... --realtime` paces by timestamps, publishes to shared memory
  and loops: a stand-in for BrokenEye when testing the layer without the
  headset.

## Limits

- 30 Hz cameras plus one frame of processing: foveation may lag after large
  saccades.
- Arrival timestamps add jitter and hide frame age.
- Python start-up and CPU use: fine so far; a C++ port is a later option.
