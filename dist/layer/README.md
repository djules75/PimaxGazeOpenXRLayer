# Pimax gaze layer: setup and use

This guide takes you from nothing to eye-tracked foveation in a game:

1. [Install the pieces](#1-install-the-pieces) (once)
2. [Calibrate for your eyes](#2-calibrate-for-your-eyes) (once, and again if accuracy drops)
3. [Check it with the follow dot](#3-check-it-with-the-follow-dot) (once, and after each calibration)
4. [Play](#4-play) (every session)

Allow about 30 minutes the first time. Afterwards a session is two steps:
start BrokenEye, then start `PimaxGaze.exe`.

**How it fits together.** BrokenEye reads the headset's eye cameras. The
**gaze service** (Python) finds your pupils in those images and turns them
into gaze angles using your calibration. The **OpenXR layer**
(`PimaxGazeLayer.dll`) gives that gaze to every OpenXR game in place of the
headset's own eye tracking. `PimaxGaze.exe` registers the layer and runs the
service for you.

```
headset cameras ─> BrokenEye ─> gaze service ─> OpenXR layer ─> game
```

## 1. Install the pieces

### What you need

- A Pimax Crystal Super with eye tracking, on Pimax OpenXR or SteamVR.
- **[BrokenEye](https://github.com/ghostiam/BrokenEye) 1.4.0.** It is
  closed-source and several antivirus engines flag it (mostly generic
  heuristics); check it yourself before running it. It must run as
  administrator.
- **Python 3.14** (the version tested) with two packages:
  ```
  py -m pip install numpy opencv-python-headless
  ```
- **This repository**, cloned or unzipped anywhere, with the built binaries in
  place:
  - `dist\layer\`: `PimaxGazeLayer.dll`, `XR_APILAYER_PIMAXGAZE_eye_gaze.json`,
    `PimaxGaze.exe`
  - `dist\calib_recorder\`: `calib_recorder.exe`, `openxr_loader.dll`

  Build steps are in the repository's main `README.md`. Keep `dist\` inside
  the repository: `PimaxGaze.exe` finds the service by searching the folders
  above its own.

### Register the layer

Run this once from `dist\layer\` (no administrator rights needed):

```
PimaxGaze.exe --register-only
```

This registers the layer for your Windows user only. While the service is
not running, games see eye tracking as *not tracked* and use fixed
foveation. To remove the layer later, run `PimaxGaze.exe --unregister`.

## 2. Calibrate for your eyes

Everyone's eyes and headset fit are different, so the service needs a
calibration file before it can run: `service\gaze_calib.json`. You record a
short session looking at dots, then fit it with one command.

### Record (about 2 minutes)

1. Start **BrokenEye as administrator** and click **Connect**. Start a
   **recording**. Preview can stay off.
2. Open a terminal in a folder where you want the session logs, and run
   `<repo>\dist\calib_recorder\calib_recorder.exe`.
3. Put the headset on and sit still. A dot appears straight ahead, then moves
   through 15 positions, twice (about 85 s).
   - **Follow the dot with your eyes only; keep your head still.** The dot
     moves with your head, so turning your head does not help.
   - Blink while the dot moves, not while it holds still.
4. The recorder closes by itself when the sweep is done. **Stop the BrokenEye
   recording.** (Escape aborts the sweep early.)

You now have two files:

- a BrokenEye recording, `stream-<date>.brokeneye`, wherever BrokenEye saves
  its recordings
- `calib-<stamp>-schedule.csv` in the terminal's folder (next to it,
  `calib-<stamp>-frames.csv`, which the fit does not need)

### Fit

1. Extract the recording into an empty folder. It is a zip file, so:
   ```
   mkdir C:\temp\calib
   tar -xf <path>\stream-<date>.brokeneye -C C:\temp\calib
   ```
   The folder should now contain `wearable\`.
2. From the repository's root folder, fit and write the calibration:
   ```
   python tools\calib_fit.py --v2 --write-calib=service\gaze_calib.json C:\temp\calib <path>\calib-<stamp>-schedule.csv
   ```
3. Read the output:
   - `holds inside the recording's time span: 32/32` (or close). If this is
     0, the recording did not overlap the sweep: record again, starting the
     BrokenEye recording before the recorder.
   - Per eye, the `pass1->pass2` figures show how well the first pass
     predicts the second. A median under 3° is good; above about 5°, record
     again and keep your head stiller.
   - It ends with `wrote service\gaze_calib.json`.

## 3. Check it with the follow dot

Before trying a game, check the whole chain:

1. Start BrokenEye as administrator, click **Connect**, and turn **Enable
   preview** on. Without preview or a recording running, BrokenEye sends no
   camera images.
2. Run `dist\layer\PimaxGaze.exe`. Its console window runs the service and
   prints how often it finds each pupil and the gaze angles, every 2 s.
3. Run `dist\calib_recorder\calib_recorder.exe --follow` and put the headset
   on. You see the 15 calibration positions as faint grey dots, and one bright
   dot where your gaze is:
   - **green:** the layer reports gaze as tracked
   - **red:** untracked (service not running, no fresh images, or not
     confident); the dot stays where it last was
4. Look at each grey dot in turn. The bright dot should land on it within
   about one or two dot widths. Escape closes the recorder.

If the dot is offset the same way everywhere, look at the centre dot and
press **Ctrl+F9** (see [If the headset moves](#if-the-headset-moves)). If it
is off in different directions at different dots, calibrate again.

## 4. Play

Every session:

1. Start **BrokenEye as administrator**, click **Connect**, turn **Enable
   preview** on. Do not leave a recording running.
2. Run **`PimaxGaze.exe`** and leave its window open. Closing it or pressing
   Ctrl+C stops the service; games then fall back to fixed foveation.
3. Start the game with eye-tracked foveated rendering enabled (for example
   CheekyFoveatedDLSS).

### If the headset moves

A calibration is exact for the position the headset was in when you
recorded it. A few millimetres of shift can move the gaze by several degrees.

- **Automatic correction** is on by default. It follows slow shifts over
  about 5 s and usually removes most of the error.
- **Ctrl+F9** (works from any window, including the game): look straight
  ahead, at a crosshair or the centre of the view, and press it. The service
  re-centres both eyes on that point immediately and pauses the automatic
  correction.
- **Ctrl+F10** undoes Ctrl+F9 and resumes the automatic correction.
- If accuracy stays poor after a big change in fit (a different gasket, a
  different strap setting), calibrate again.

## Troubleshooting

| Symptom | Likely cause and fix |
|---|---|
| `PimaxGaze.exe` says `no calibration` | Do [step 2](#2-calibrate-for-your-eyes) first, or pass `--calib <file>`. |
| The service window shows no detections | BrokenEye is not running as administrator, not connected, or preview is off. |
| The follow dot stays red | Same as above, or the service is not running. |
| The game says no eye tracker, or ignores your gaze | Check the layer log (below) for `app enabled eye gaze`. Make sure the game's foveation mod is set to use eye tracking. |
| The gaze is offset everywhere by the same amount | Headset shifted: press Ctrl+F9 while looking straight ahead. |
| Easy Anti-Cheat games | The layer is not code-signed yet; EAC refuses to load it. |

**Logs:**

- The layer writes `%LOCALAPPDATA%\PimaxGazeLayer\layer-<pid>.log`, one per
  game process. It shows whether the game asked for eye gaze and, every 5 s,
  how many gaze requests got your gaze (`overridden`), were not confident
  enough (`untracked`) or found no service (`no-service`).
- The service prints to the `PimaxGaze.exe` window.

## Reference

`PimaxGaze.exe` options:

- `--calib <file>`: use another calibration file (default
  `<repo>\service\gaze_calib.json`)
- `--python <exe>`: which Python to run (default `python.exe` on PATH, else
  `py -3`)
- `--repo <dir>`, `--manifest <json>`: override where it looks for the
  service and the layer manifest
- `--no-register`, `--register-only`, `--unregister`
- anything else is passed to the service, for example `--no-auto` (turn the
  automatic correction off), `--auto-tau S` (its time constant),
  `--host`/`--port` (BrokenEye's address, default 127.0.0.1:5555)

`PimaxGaze.exe` also removes registrations of other copies of the layer, so
only one loads, and refuses to start a second service while one is running.

Without the launcher, the same steps by hand:

```powershell
powershell -ExecutionPolicy Bypass -File <repo>\layer\register.ps1 -ManifestPath <repo>\dist\layer\XR_APILAYER_PIMAXGAZE_eye_gaze.json
python <repo>\service\gaze_service.py --calib <repo>\service\gaze_calib.json
```

`layer\unregister.ps1` removes the registration.

Environment variables, set for a game:

- `PIMAXGAZE_OVERRIDE=0`: the layer only logs and passes the headset's own
  gaze through.
- `PIMAXGAZE_LAYER_DISABLE=1`: the OpenXR loader skips the layer entirely.
