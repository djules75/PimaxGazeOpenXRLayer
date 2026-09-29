# Environment: machine, runtimes, other layers

## Machine

- Windows 11 Pro.
- Headsets: Pimax Crystal Super (4172×3268 per eye) and a Quest 3.
- **Python** 3.14 with Pillow, NumPy and `opencv-python-headless` 5.0.
- **Build:** Visual Studio 17 2022 through CMake.

## Pimax and Tobii software

- Pimax Play (`PimaxClient.exe`) with `pi_server.exe`, `pi_overlay.exe`,
  `pi_vst.exe`, all under `C:\Program Files\Pimax\`.
- Tobii runtimes: the `Tobii VR4PIMAXP3B Platform Runtime` service (original
  Crystal's tracker) and the `platform_runtime_XR5EYECHIP_WIN10_x64` process
  (this headset's tracker), which runs elevated.
- Pimax's own foveated rendering reads Tobii internally and never sees this
  layer's gaze. Out of reach.

## OpenXR runtimes (measured)

- The user switches between **Pimax OpenXR** (`C:\Program Files\Pimax\Runtime\PiOpenXR_64.json`,
  runtime name `Pimax OpenXR`, reports 0.1.0), **SteamVR** (drives the Pimax;
  the latest sboys3 driver supports eye tracking) and **VDXR** (Quest 3). The
  community PimaxXR is not installed.
- **Both Pimax OpenXR and SteamVR already expose `XR_EXT_eye_gaze_interaction`**
  (Pimax's DLL also carries `XR_VARJO_quad_views` and
  `XR_VARJO_foveated_rendering`). So the layer overrides; it never adds the
  extension.
- **The runtime's gaze is worthless:** flagged valid and tracked even with
  nobody wearing the headset (flags 15, `isActive` 1, constant −5.2°/−1.2°);
  during calibration it changed on only 34% of app frames and had a 12° median
  error against the targets.

## Other implicit layers

- **HKLM:** OpenXR Toolkit (disabled), Virtual Desktop's Oculus compatibility
  layer, and `XR_APILAYER_CHEEKY_foveated_dlss` (CheekyFoveatedDLSS 0.5.1, an
  "OpenXR eye-gaze bridge"). Cheeky binds
  `/interaction_profiles/ext/eye_gaze_interaction` and
  `/user/eyes_ext/input/gaze_ext/pose` and calls `xrLocateSpace` and
  `xrGetActionStatePose`: a standard consumer. Quad-Views-Foveated is not
  installed.
- **HKCU:** this layer.
- **Ordering (measured):** the loader reads HKLM implicit layers before HKCU,
  and earlier means nearer the app, so this HKCU layer sits between Cheeky and
  the runtime, which is what a consumer needs. Confirmed in the Cyberpunk 2077
  run ([openxr-layer.md](openxr-layer.md)); `XR_LOADER_DEBUG=all` shows the
  chain (output goes to the app's stdout).
- **Cheeky baseline:** `%APPDATA%\UnrealVRMod\HogwartsLegacy\CheekyFoveatedDLSS-UEVR.log`
  (2026-09-26) shows ~1,170 "VR gaze history reset" events in 39 min, about
  one every 2 s. Its "CALIB" lines are about DLSS view mapping, not eyes.

## Anti-cheat: EAC refuses unsigned layers (measured)

A kernel image-load trace of Star Citizen (Easy Anti-Cheat) showed the loader
mapping unsigned layer DLLs (Cheeky's among them) and the process unloading
each 0.4–0.5 ms later, before its entry point ran, while Virtual Desktop's
signed layer stayed loaded. Star Citizen is the user's first candidate for
eye-tracked foveation, so this layer needs a Windows-trusted, timestamped
code-signing certificate there; whether EAC then accepts it without an
allowlist entry is untested.
