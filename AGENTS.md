# Pimax Gaze Layer: notes for agents

Camera-based eye gaze for the user's Pimax Crystal Super, whose built-in Tobii
tracker rejects most samples since they fitted a custom face gasket. A Python
service estimates gaze from the raw IR eye-camera images, and an OpenXR
implicit API layer hands that gaze to every OpenXR app in place of the
runtime's.

Start here, then open the topic file you need. Statements are marked
**measured** (seen in a recording or on this machine) or **inferred** (a
reasoned guess, not verified) where it matters; treat inferred items as
hypotheses to test first.

## The problem and the fixed constraint

- **Headset:** Pimax Crystal Super with a built-in Tobii eye tracker (XR5
  generation).
- **The change:** the user 3D-printed a face gasket that brings their eyes
  closer to the lenses, for the field of view they want. Since then Tobii
  tracks only occasionally (5–15% of samples valid).
- **The gasket stays.** The user tried and rejected spacers. Never propose
  moving the eyes back; the solution must work at the current eye position.
- **Cause (inferred):** the camera images are fine; Tobii discards samples
  whose reconstructed 3D eye position falls outside its designed eye box.
  Tobii's runtime is closed, so a replacement estimator on the raw images is
  the only lever. Evidence: [agents/diagnosis.md](agents/diagnosis.md).
- **Goal:** usable gaze for **any OpenXR consumer of
  `XR_EXT_eye_gaze_interaction`**, first of all eye-tracked foveated rendering
  (CheekyFoveatedDLSS is one consumer, not the only one), on both Pimax OpenXR
  and SteamVR. VRChat and VRCFaceTracking are not targets.

## How it works

```
Tobii XR5 runtime ──> BrokenEye (admin, preview on) ──RAW TCP──> gaze service ──shared memory──> OpenXR layer ──> app
```

- **BrokenEye** (third-party, closed, archived) is the only source of the eye
  camera images. It must run as administrator with preview on.
  [agents/brokeneye.md](agents/brokeneye.md)
- **Gaze service** (`service/gaze_service.py`): pupil detection per camera,
  per-eye calibrated map to gaze angles, two-eye fusion, slip correction,
  publishes to shared memory `Local\PimaxGazeLayer`.
  [agents/gaze-service.md](agents/gaze-service.md)
- **OpenXR layer** (`layer/`, `PimaxGazeLayer.dll`): HKCU implicit layer that
  overrides the eye-gaze pose for every app; reports untracked when the
  service is absent, stale or unsure. [agents/openxr-layer.md](agents/openxr-layer.md)
- **Calibration** (`tools/calib_recorder/`, `tools/calib_fit.py`): a 15-target
  head-locked sweep recorded alongside BrokenEye, fitted into
  `service/gaze_calib.json`. [agents/calibration.md](agents/calibration.md)
- **Launcher** (`tools/launcher/`, `PimaxGaze.exe`): registers the layer and
  runs the service; never touches BrokenEye.

## Repository map

| Path | What |
|---|---|
| `service/gaze_service.py` | Gaze service, live and replay. Shared-memory layout in its docstring. |
| `service/gaze_calib.json` | Current calibration (gitignored, personal). |
| `layer/` | Layer source, manifest, `register.ps1` / `unregister.ps1`. |
| `tools/launcher/` | `PimaxGaze.exe` source. |
| `tools/calib_recorder/` | OpenXR calibration and `--follow` test app. Has its own README. |
| `tools/calib_fit.py` | Scores recordings and writes calibrations (`--v2 --write-calib`). |
| `tools/pupil_v2.py` | Current pupil detector (imported by the service). |
| `tools/pupil_probe.py`, `tools/gaze_fit.py` | First prototypes; superseded, kept for reference. |
| `dist/layer/`, `dist/calib_recorder/` | Binaries the user runs (gitignored except the READMEs). `dist/layer/README.md` is the user's run guide. |
| `evidence/` | Plots and the tracker's `device.json` from the diagnosis. |
| `calib-*-{schedule,frames}.csv` | Recorder sessions (gitignored). |

Machine, runtimes, other layers and EAC:
[agents/environment.md](agents/environment.md).

## Building

All C++ is MSVC via CMake, Visual Studio 17 2022, x64, Release. The layer and
the recorder download the Khronos OpenXR-SDK 1.1.61 headers at configure time
(pinned by hash); `-DPGZ_OPENXR_SDK_ROOT=` (layer) or
`-DXRCAL_OPENXR_SDK_ROOT=` (recorder) points them at a local SDK instead.

```
cmake -S layer -B layer/build -G "Visual Studio 17 2022" -A x64
cmake --build layer/build --config Release
cmake -S tools/launcher -B tools/launcher/build -G "Visual Studio 17 2022" -A x64
cmake --build tools/launcher/build --config Release
```

Copy the outputs into `dist/layer/` (DLL, manifest, `PimaxGaze.exe`). The
recorder's build is in `tools/calib_recorder/README.md`. Python 3.14 with
NumPy and `opencv-python-headless` runs the service and tools.

## Status (2026-09-29)

- **Working live (measured, user-confirmed):** BrokenEye → service → layer →
  `calib_recorder --follow`; the dot follows the gaze "quite well".
- **Games (user-confirmed, 2026-09-29):** Microsoft Flight Simulator 2024 with
  its native eye-tracked foveated rendering works perfectly on both Pimax
  OpenXR and SteamVR; Cyberpunk 2077 through R.E.A.L. VR with Cheeky works on
  SteamVR. Details in [agents/openxr-layer.md](agents/openxr-layer.md).
- **Accuracy:** calibration from pass 1 scored on pass 2 through the service:
  median 1.3°, p90 3.1°, all 15 targets to ±30° yaw / ±20° pitch covered.
  After a headset shift, the automatic slip correction brings a stale
  calibration from 9.6° to 3.8° median; Ctrl+F9 re-centres instantly.
- **Camera rate is 30 Hz per eye** with no timestamps on the live stream;
  the runtime's own gaze is worthless (stale, 12° median error), so the camera
  estimate is the primary source, not a fallback.
- **BrokenEye is the camera source**, a third-party closed-source tool used
  as is. See [agents/brokeneye.md](agents/brokeneye.md).

## Next steps

1. **Cheeky on Pimax OpenXR, and its gaze-reset count:** MSFS 2024 works on
   both runtimes and Cheeky works on SteamVR (layer order confirmed). Still
   open: a Cheeky title on Pimax OpenXR, and comparing Cheeky's log against
   the baseline (~1,170 "gaze history reset" events in 39 min). Collect the
   layer log, the service log and Cheeky's log.
2. **Slip over a long session:** pass-to-pass numbers cover ~40 s of wear.
   Watch accuracy over a real session before deciding on glint or eye-corner
   referencing.
3. **Latency:** 30 Hz plus one frame of processing. If foveation visibly lags
   after saccades, look for a way to get more than 30 Hz.
4. **Code signing** before Star Citizen or any EAC title: EAC unloads unsigned
   layers ([agents/environment.md](agents/environment.md)).
5. **Later:** a tray app, and a C++ service if Python's start-up or CPU use
   gets in the way.
