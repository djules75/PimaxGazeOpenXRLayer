# Pimax Gaze Layer

Camera-based eye gaze for the Pimax Crystal Super when its built-in Tobii
tracker stops tracking because the eyes sit closer to the lenses than Tobii
expects, for example with a thinner or custom face gasket.

The eye cameras still see the pupils clearly, but Tobii rejects most samples
(5–15% valid in our tests). This project estimates gaze directly from the raw
IR eye-camera images and hands it to every OpenXR app through
`XR_EXT_eye_gaze_interaction`, in place of the runtime's gaze. The main use is
eye-tracked foveated rendering (for example CheekyFoveatedDLSS), on Pimax
OpenXR and SteamVR.

```
Tobii runtime ─> BrokenEye ─> gaze service (Python) ─> shared memory ─> OpenXR layer ─> app
```

- **Gaze service** (`service/`): pupil detection per camera, a per-eye
  calibrated map to gaze angles, two-eye fusion and headset-slip correction.
- **OpenXR layer** (`layer/`): a per-user implicit API layer that overrides the
  eye-gaze pose. When the service is absent, stale or unsure it reports gaze as
  not tracked, so apps fall back to fixed foveation.
- **Calibration** (`tools/calib_recorder/`, `tools/calib_fit.py`): a 15-target
  sweep recorded in the headset, fitted into a personal calibration file.
- **Launcher** (`tools/launcher/`, `PimaxGaze.exe`): registers the layer and
  runs the service.

Accuracy measured on one user: median 1.3°, p90 3.1° across ±30° yaw and
±20° pitch. The cameras run at 30 Hz per eye.

## Status and limits

This is a personal project, tested on one headset and one person. Expect to
build it yourself and calibrate for your own face.

**Tested with:**

- Microsoft Flight Simulator 2024, using its built-in eye-tracked foveated
  rendering, on both Pimax OpenXR and SteamVR.
- Cyberpunk 2077 through R.E.A.L. VR with CheekyFoveatedDLSS, on SteamVR.

- **It depends on [BrokenEye](https://github.com/ghostiam/BrokenEye)**, a
  closed-source, archived third-party tool and the only available source of
  the camera images. It must run as administrator with preview on. Several
  antivirus engines flag it (mostly generic heuristics on its obfuscated
  binary); check it yourself before running it.
- **Anti-cheat:** Easy Anti-Cheat unloads unsigned OpenXR layers, and this
  layer is not code-signed yet.

## Building

Windows, Visual Studio 2022, CMake 3.24+. The first configure downloads the
OpenXR headers ([Khronos OpenXR-SDK](https://github.com/KhronosGroup/OpenXR-SDK)
1.1.61), so it needs an internet connection.

```
cmake -S layer -B layer/build -G "Visual Studio 17 2022" -A x64
cmake --build layer/build --config Release
cmake -S tools/launcher -B tools/launcher/build -G "Visual Studio 17 2022" -A x64
cmake --build tools/launcher/build --config Release
```

Copy `PimaxGazeLayer.dll`, `layer/XR_APILAYER_PIMAXGAZE_eye_gaze.json` and
`PimaxGaze.exe` into `dist/layer/`. The calibration recorder has its own build
notes in `tools/calib_recorder/README.md`.

The service and tools need Python 3.14 with NumPy and
`opencv-python-headless`.

## Running

[dist/layer/README.md](dist/layer/README.md) covers setup, calibration, the
live check and each play session.

## Documentation

[AGENTS.md](AGENTS.md) and [agents/](agents/) hold the design notes, the
measurements behind each decision and the diagnosis of why Tobii fails. They
are written for coding agents, and people can read them too.

## Licence

MIT, see [LICENSE](LICENSE). Not affiliated with Pimax or Tobii.
