# The OpenXR layer (`layer/`)

`PimaxGazeLayer.dll`, manifest `XR_APILAYER_PIMAXGAZE_eye_gaze.json`, an
implicit layer registered per user under
`HKCU\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit` (by `PimaxGaze.exe`, or
`register.ps1` / `unregister.ps1`). No admin needed.

## What it does

Both target runtimes already expose `XR_EXT_eye_gaze_interaction`
([environment.md](environment.md)), so the layer only **overrides** the gaze
pose; it never adds the extension.

- `xrSuggestInteractionProfileBindings`: records actions bound to
  `/user/eyes_ext/input/gaze_ext/pose` under
  `/interaction_profiles/ext/eye_gaze_interaction`.
- `xrCreateActionSpace`: remembers spaces made from those actions.
- `xrLocateSpace` / `xrLocateSpaces` (the latter only when the next layer has
  it): for a gaze space, calls down, then composes the service's yaw/pitch
  onto the head pose from a private `VIEW` space and reports tracked.
- `xrGetActionStatePose`: `isActive` follows whether the service is alive.
- Session, space and instance bookkeeping; the layer's own handles never go
  down to the runtime.

## Failing open

Gaze is reported **untracked** (tracked bits cleared) when:

- the service is absent, or its heartbeat is older than 1.5 s;
- the newest camera frame used is older than 200 ms;
- confidence is below 0.5.

Consumers then fall back to fixed foveation. Confidently wrong gaze blurs
exactly where the user looks, which is worse than no eye tracking. The layer
never blocks and never returns an error the runtime would not.

## Shared memory

Reads `Local\PimaxGazeLayer` (88-byte block, seqlock on `seq`; layout in the
docstring of `service/gaze_service.py` and `GazeBlock` in `gaze_layer.cpp`,
which must match). Opens it lazily, retrying once a second.

## Knobs and logs

- `PIMAXGAZE_OVERRIDE=0`: log only, pass the runtime's gaze through.
- `PIMAXGAZE_LAYER_DISABLE=1`: the loader skips the layer (manifest
  `disable_environment`).
- Log: `%LOCALAPPDATA%\PimaxGazeLayer\layer-<pid>.log`: whether the app bound
  eye gaze, then every 5 s the counts of overridden, untracked and no-service
  locates.

## Verified

- Headset idle, service in real-time replay, `calib_recorder` as the app on
  Pimax OpenXR: negotiated, recorded binding and space, overrode locates with
  flags 15, the app saw the replayed sweep.
- Live with the headset, via `calib_recorder --follow`: confirmed by the user.
- SteamVR: works (user, 2026-09-29).
- **Microsoft Flight Simulator 2024**, native eye-tracked foveated rendering
  (no Cheeky), on Pimax OpenXR and on SteamVR: works perfectly (user,
  2026-09-29).
- **Loader order relative to Cheeky** (2026-09-29, Cyberpunk 2077 through
  R.E.A.L. VR, SteamVR): Cheeky sits above this layer, nearer the app.
  Cheeky's `xrCreateApiLayerInstance` probes the *next*
  `xrGetInstanceProcAddr` with `XR_NULL_HANDLE` for
  `xrEnumerateInstanceExtensionProperties` to decide whether to enable
  `XR_EXT_eye_gaze_interaction`. This layer answered `XR_ERROR_HANDLE_INVALID`
  (no instance yet), Cheeky dropped the extension, the log read
  `app enabled eye gaze: no` and Cheeky showed "Eye tracker not detected".
  `resolve_global_function` now answers that query from the loader's exported
  `xrGetInstanceProcAddr`, else - R.E.A.L. VR links the loader statically into
  its `dxgi.dll`, so nothing exports it - from the already-loaded runtime via
  `xrNegotiateLoaderRuntimeInterface`. Each answer is logged
  (`null-instance ...: answered from the loader|runtime`). Confirmed:
  `app enabled eye gaze: yes`, Cheeky "ready".
- **Cheeky's gaze space was never overridden** (same run): Cheeky creates
  its gaze action space in `xrCreateSession` and suggests the binding later,
  at attach; the layer only recognised spaces whose action was already bound,
  so no `gaze action space created` line, no `locates` report, and Cheeky read
  SteamVR's own gaze pose. Action spaces are now kept pending and promoted
  when the binding arrives (`gaze action space promoted at binding`).
- **The runtime's pose must not stay VALID.** Cheeky accepts any location
  with `ORIENTATION_VALID`; clearing only the TRACKED bits on a dropout let
  the runtime's placeholder through, and at 30-60% pupil detection the
  foveation jumped between the two (Cheeky `VR gaze history reset reason=5`,
  large_jump, even while the service saw nothing). Now: the last good gaze is
  held 250 ms (valid, not tracked; `held` counter), then the location is
  reported with no flags. Confirmed in game (MSFS 2024 and Cyberpunk 2077,
  user, 2026-09-29).

## Open items

- Code signing for EAC titles ([environment.md](environment.md)).
- Consider `XR_FB_eye_tracking_social` only if a consumer the user cares about
  needs it.
- Check whether Quad-Views-Foveated (not installed now) reads gaze through
  `XR_EXT_eye_gaze_interaction` on a Pimax; if not, it needs another path.

## Operational: stale Broken Eye instances (2026-09-29)

`Broken Eye.exe` does not exit cleanly; ten instances were found running,
the oldest two days old, only one owning 127.0.0.1:5555. The others compete
for the eye cameras: pupil detection collapsed to `0/61` for stretches, and
the service's connection was refused once. Before a session: close every
Broken Eye process, start one, then PimaxGaze.
