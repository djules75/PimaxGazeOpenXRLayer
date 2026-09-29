# Calibration recorder

Shows a fixation target at 15 known head-relative angles (yaw up to ±30°,
pitch up to ±20°), two passes, about
85 s in total, and logs the schedule on the same clock BrokenEye records.
Together with a BrokenEye recording this gives camera frames labelled with the
gaze angle we asked for, independent of Tobii.

## Running a session

1. Start **BrokenEye as administrator**, Connect, leave preview off, and
   **start a recording**.
2. Open a terminal in the folder you want the logs in and run
   `calib_recorder.exe` (the `openxr_loader.dll` next to it is required). It
   prints the runtime name and whether the runtime offers eye gaze, then waits
   for the session.
3. Put the headset on. The dot appears straight ahead and holds for 3 s, then
   moves through 15 positions, holding each for 2 s, twice, then returns to the
   centre. **Follow it with the eyes only; keep the head still.** Blink when it
   moves, not while it holds.
4. The app exits by itself when done. **Stop the BrokenEye recording.**
   Escape aborts early.
5. Fit the calibration from the `.brokeneye` recording and
   `calib-<stamp>-schedule.csv`: see "Calibrate for your eyes" in
   `dist/layer/README.md`.

Options: `--passes N` (default 2), `--hold S` (2.0), `--move S` (0.6),
`--distance M` (1.5, where the dot is placed).

## Analysis

```
python tools/calib_fit.py <extracted recording dir> calib-<stamp>-schedule.csv calib-<stamp>-frames.csv out
```

## Notes

- The target is head-locked (rendered in the VIEW reference space), so head
  motion does not change the requested eye angle.
- The app sets `CHEEKY_OPENXR_LAYER_DISABLE=1` for itself so the Cheeky bridge
  layer stays out of the process; it reads the runtime's own eye gaze through
  `XR_EXT_eye_gaze_interaction` and logs it per frame.
- Angles: yaw positive to the right, pitch positive up, in degrees, head frame.

## Building

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DXRCAL_OPENXR_LOADER_DLL="<path to openxr_loader.dll>"
cmake --build build --config Release
```

The first configure downloads the Khronos OpenXR-SDK 1.1.61 headers (set
`XRCAL_OPENXR_SDK_ROOT` to use a local SDK instead). The recorder also needs
an `openxr_loader.dll` beside it; SteamVR ships one in
`<Steam>\steamapps\common\SteamVR\bin\win64\`, and
`XRCAL_OPENXR_LOADER_DLL` copies it there after each build.
