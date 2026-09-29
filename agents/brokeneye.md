# BrokenEye: the camera source

## What it is

- https://github.com/ghostiam/BrokenEye, version 1.4.0 (app build 1.4.0.28).
  Closed source (the repository holds only a README), Go with the Fyne GUI,
  obfuscated with garble. The repository is **archived**; the author has no
  public contact.
- It reaches Tobii's advanced data and the raw IR camera images. v1.4.0 added
  XR5 support, which covers the Crystal Super.
- **Release checked:** `Broken.Eye_v1.4.0.zip`
  (SHA-256 `3dabcded4dec73450dc4d3f61ac6fde6af5cd13eee43b0fd71ee8651df43f3d7`,
  matches the release). Exe SHA-256
  `c5e47afc30fb578016c770c421385057fe76bb3e1b8e8a33487bb20e007d81b9`.
- **Security vetting:** Defender flags `Trojan:Win32/Suschil!rfn` (heuristic);
  VirusTotal 15/71, almost all generic/ML plus ClamAV `Win.Tool.Garble`;
  Kaspersky `Trojan.Win64.Agent.smgaxn` is the only notable one. The sandbox
  saw no outbound traffic beyond one DNS lookup, no startup entries.
- **Known risk:** issue #27 reports a BSOD (`usbvideo!CaptureProcessDataPayload`,
  0xD1) on a Crystal Super, blamed on a failing cable. Keep in mind for anything
  that pulls frames continuously.

## Running it (measured)

- Start it **as administrator**; otherwise Connect fails with
  `TOBII_ERROR_CONNECTION_FAILED`.
- **Preview (or a recording) must be on.** With both off it accepts TCP
  connections and sends nothing on any request, HTTP included. Preview is
  enough for live use; do not leave a recording running (a 15-minute recording
  makes offline replays slow).
- Our tools never start or control BrokenEye; the user does.

## Outputs

- **HTTP MJPEG** (images only): `http://127.0.0.1:5555/eye/left`, `/eye/right`,
  `/` for both.
- **RAW TCP API** on the same port (checked against the README and
  https://github.com/ghostiam/VRCFTTobiiAdvanced/blob/main/BrokenEye/Client.cs):
  - Send one request byte per connection: `0x00` eye data as JSON, `0x01` left
    raw images, `0x02` right raw images. The service opens one connection per
    camera.
  - Each message: id byte, 4-byte **little-endian** length (Client.cs's comment
    says big-endian; its code decodes little), payload.
  - Raw image payload: width, height, bits per pixel (4 bytes each), pixels.
    Frames are 200×200 8-bit.
  - JSON payload: per eye `gaze_direction`, `pupil_diameter_mm`,
    `pupil_position_on_image`, `openness`, each with `_is_valid`. No gaze origin.
  - **No timestamps** on either stream; live frames are stamped on arrival.
  - Loop until the full payload is read (Client.cs's single read is fragile).
- **OSC server** for eye openness (not used).

## Recordings (`.brokeneye`, a zip)

- `device.json` (tracker: `XR5_PIMAX_DA_K11_RAWH`, firmware
  `df9b4b881f-7.32.0`, runtime `1.21.0.0_8b8e96a33e`; copy in `evidence/`).
- `wearable/advanced/<TimestampTrackerUs>.json`: full Tobii sample at 121 Hz.
  Per eye `GazeOriginMmXyz`, `GazeDirectionNormalizedXyz`, `PupilDiameterMm`,
  `PositionGuideXy`, `Blink`, each with a `*Validity` flag; combined
  `GazeDirectionCombinedNormalizedXyz`, `ConvergenceDistanceMm`,
  `ImproveUserPositionHmd`.
- `wearable/image/<ts>.json` plus `<ts>-<camera>.png`, 200×200 greyscale.
  **`CameraID` 0 is the left eye, 1 the right.**
- The cameras are timestamped independently; pair each frame with the nearest
  sample rather than expecting left/right pairs.
- A few PNGs are 33-byte empty files; skip them.
- **`TimestampSystemUs` is the QPC clock in microseconds** (verified against
  uptime), the same clock `calib_recorder` logs, so recordings and schedules
  align directly (residual p95 1.4 ms).
- Extract into a scratch folder, never over the original recordings.
