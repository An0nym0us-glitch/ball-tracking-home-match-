# Pocket Hawk-Eye — Plan & Architecture (draft for review)

Status: **proposal, nothing built yet.** Items marked **[DECIDE]** need your OK before code is written.

Working name: `pocketeye` (placeholder, easy to rename) **[DECIDE]**

---

## 0. Camera setup (confirmed)
- **Exactly one camera, front view only:** the phone's back camera, mounted behind the bowler's stumps, facing the batsman (the batsman's front). There is no side camera, no second phone and no camera behind the batsman.
- Every metric below is computed from this single front view. The engine is designed for one camera only, so it never waits for a second view.

## 1. Key findings that shape the design

### 1.1 Flutter cannot run on the SM-G531F
- Flutter dropped API 19 in 3.22 (minSdk 21) and **raised the minimum to API 24 (Android 7.0) in 3.35**. Current plugins are moving to minSdk 24 as well.
- The SM-G531F runs Android 5.1.1 = **API 22**, so current Flutter won't run on it.
- Options for the old-phone camera side:

| Option | Pros | Cons | Verdict |
|---|---|---|---|
| **A. Tiny native Kotlin camera/stream app** (`camera_lite/`, minSdk 21, *no AndroidX*: Camera1 API + MediaCodec + plain sockets) | Small (<1 MB). Sends the real capture timestamp with every frame. We control the encoder bitrate, fps lock and exposure hints. QR pairing + result card. | A second small codebase (~1.5k lines) | **Recommended** |
| B. Pin an old Flutter (≤3.32) for the phone app | One UI codebase | Stuck on an old Flutter forever; plugins drift away; heavy app for 1 GB RAM | Not recommended |
| C. Existing IP-camera app (e.g. IP Webcam, MJPEG/RTSP) | Zero work, good for early experiments | Frames carry arrival time, not capture time (timing jitter → speed error); MJPEG needs ~20+ Mbps on 2.4 GHz; no result card | **Prototype only** |

For Step 1 (engine on real footage), the fastest route is simpler still: **record with the phone's stock camera app and copy the MP4 to the laptop.**

Note: recent AndroidX/CameraX releases have moved to minSdk 23, which is why `camera_lite` uses only framework APIs.

### 1.2 What one camera behind the bowler can and can't measure
The camera looks straight down the pitch, so **depth (along the pitch) is the weak axis**:
- Example: camera 1.2 m high, f ≈ 1000 px (720p, ~65° HFOV). At 15 m, 1 px of vertical image error ≈ **19 cm** of ground distance. At 9 m it's ≈ 8 cm.
- The ball is tiny. A 6.5 cm ball at 15 m is about **4–5 px** across at 720p. Ball size alone is a weak depth cue at the far end, but a good one near the bowler.
- **What saves us:** the full 3D trajectory is constrained by three things:
  1. **Gravity is known (9.81 m/s²).** It gives a metric scale to the ball's vertical motion over known frame times.
  2. **The bounce point lies on the calibrated ground plane.** This gives a hard 3D anchor.
  3. **Apparent ball size** (radius from the ball type), weighted by its uncertainty.

  The fit combines all three. The across-pitch (line) axis is the strong axis: it maps almost directly to image x.
- **Motion blur:** the old phone has no manual shutter control (Camera1 / LEGACY Camera2). The ball will often be a **streak**, not a circle. The detector must accept elongated blobs (roundness filter relaxed along the motion direction, using the streak centre and length). It must not reject them.
- **Frame timing:** speed is distance divided by time, so every frame needs its *capture* timestamp, not its arrival time. Variable frame rate (common in low light) is handled by using the real per-frame timestamps throughout.
- **Camera height:** a higher camera (1.5–2 m) clearly improves length accuracy. The app will show this as a tip.

---

## 2. Architecture overview

```
           ┌─────────────────────── Phone-only mode (API 24+, strong phone) ───────────────────────┐
           │ Flutter app ──► native Camera2 plugin (Kotlin) ──JNI──► pocketeye engine (C++) ──► result │
           └─────────────────────────────────────────────────────────────────────────────────────────┘

 ┌────────── Phone + laptop mode ──────────┐            ┌──────────────── Laptop (Windows) ───────────────────┐
 │ camera_lite (API 21+)  or Flutter app   │  H.264 +   │ pe_host (C++): TCP rx ► HW decode (FFmpeg d3d11va)   │
 │ capture ► HW H.264 encode ► TCP stream  │──per-frame─►│   ► ring buffer ► pocketeye engine ► result JSON     │
 │ shows result card ◄─────────────────────│◄─ control ─│ Flutter Windows UI (FFI): calibration, cards, replay │
 └─────────────────────────────────────────┘  (JSON)    └──────────────────────────────────────────────────────┘
```

- **One engine, many front ends.** All tracking code lives in `engine/` (C++17). It is used by the CLI, the Windows app (through a C API + dart:ffi) and the Android app (through JNI and FFI). The engine takes **decoded frames plus timestamps**, not files, so its results don't depend on the platform. Decoding stays at the edges: FFmpeg on desktop, MediaCodec on Android.
- **Deterministic core.** The same frames and calibration always give the same JSON. CI will test this.

### 2.1 Engine pipeline (classical first)
| Stage | Method |
|---|---|
| Pitch ROI | Mask from calibration: a corridor over the pitch, extended up to ~2.5 m. Everything else is ignored. |
| Delivery trigger | Runs at low resolution (≈320×180) on every frame. It looks for a small moving blob that travels from the bowler end toward the batsman with consistent direction. It opens a delivery window over a ring buffer (last ~3 s). |
| Motion detection | Running-median background + 3-frame differencing. Adaptive threshold. Morphology. |
| Candidate filter | Blob area range from the expected ball size at that depth (from calibration + ball type), roundness / streak model, colour hint from the ball type (white tennis, red leather, etc.). |
| Tracking | Constant-acceleration Kalman filter in image space. Gating + best-path selection (multi-hypothesis over a short window) to reject hands, shadows and birds. |
| Events | **Bounce:** vertical-velocity sign flip near the ground plane, refined by intersecting the pre- and post-bounce fits (sub-frame). **Bat/pad contact:** abrupt change of velocity off the ground near the batsman's crease. |
| 3D fit | Levenberg–Marquardt over a segment model. Pre-bounce: `p0, v0, side-accel, along-drag` with gravity fixed. Bounce on plane `z = r_ball`, continuous position. Post-bounce: new velocity, same accelerations. The residual is reprojection error, plus an apparent-radius term. The fit outputs a covariance. |
| Uncertainty | Monte-Carlo sampling from the fit covariance. This gives error bars on every metric and P(hitting) for wicket prediction. |

### 2.2 Metric definitions
Coordinates: origin at the base of the bowler-end middle stump. **y** points along the pitch toward the batsman, **x** points to the off side of the batsman (needs a right/left-handed setting **[DECIDE]**), **z** points up.

| Metric | Definition |
|---|---|
| Release speed | \|v\| of the fitted model at the first reliable detection (the bowler hides the actual release), reported in km/h |
| Average speed | Fitted path length ÷ flight time, from the first detection to the batsman's stumps line (or to contact) |
| Swing | Sideways deviation at the bounce relative to the straight line of the initial direction: ½·a_side·t² (cm). Shown as in-swing / out-swing relative to the batsman. |
| Turn | Angle between the horizontal velocity directions before and after the bounce (degrees, off / leg) |
| Line & length | Bounce point (x, y). Length is measured from the batsman's stumps, with zones (yorker / full / good / short) scaled to *your* pitch length, not fixed 22-yard values. |
| Bounce height at stumps | Fitted z where y = batsman's stumps line |
| Wicket prediction | Project the path to the stumps plane and test it against the stump box (width + height from calibration, plus the ball radius). **Hitting:** P ≥ 0.9. **Missing:** P ≤ 0.1. **Close call:** in between, or within one ball width of the edge. If the ball is stopped before the stumps, we extrapolate. If there are too few post-bounce points, the result is "not enough data". |
| Shot power *(approximate)* | Exit speed off the bat from the post-contact track (often only a few frames, with size change as the depth cue). Shown as a 0–100 rating, normalised per ball type, plus a direction. **Always labelled approximate.** |

### 2.3 Honest accuracy estimates (to be *measured*, not promised)
These are pre-build engineering estimates for a well-calibrated setup with a camera at about 1.2 m and a 15–20 m pitch. Step 1 replaces them with measured numbers from the synthetic and real tests. The app will show the measured error bars per metric.

| Metric | 720p @ 30 fps (SM-G531F) | 1080p @ 60 fps (Redmi Note 10S) | Confidence |
|---|---|---|---|
| Average speed | ±4–7 % | ±2–4 % | Good |
| Release speed | ±6–10 % | ±3–6 % | Fair (extrapolated) |
| Swing | ±8–15 cm | ±4–8 cm | Fair |
| Turn | ±2–4° (may be "not measurable" on full balls: only 2–4 frames after the bounce) | ±1–2° | Fair / weak at 30 fps |
| Length (bounce y) | ±20–40 cm | ±10–25 cm | Fair (foreshortening) |
| Line (bounce x) | ±3–6 cm | ±2–4 cm | Good |
| Bounce height at stumps | ±6–12 cm | ±3–8 cm | Fair |
| Wicket prediction | close-call band ≈ ±1 ball width | ≈ ±½ ball width | Fair |
| Shot power | ±25–35 % | ±15–25 % | **Approximate** |

No AI frame interpolation anywhere. Low fps is reported as a wider error bar.

---

## 3. Tech stack **[DECIDE]**

| Area | Choice | Why |
|---|---|---|
| Engine | C++17, CMake, **OpenCV** (core, imgproc, video, calib3d: minimal build), **Eigen** + our own small LM solver (≈10 parameters, no Ceres), nlohmann/json | Fast, portable to Windows, Android NDK and Linux CI |
| Engine tests | GoogleTest (math), plus Python end-to-end accuracy gates | Required tests for speed, swing, turn and wicket math |
| Video I/O (desktop) | FFmpeg (LGPL, dynamic) with hwaccel **d3d11va** (works on Intel UHD and RTX). CPU fallback. | Hardware decode where possible. At 720p30 software decode is also cheap, so hardware decode matters mainly for 1080p60. |
| Video I/O (Android) | MediaCodec (encode in camera_lite, decode for clips in the Flutter app) | Hardware, no extra libraries |
| UI | Flutter (Windows + Android API 24+). `dart:ffi` bindings generated with `ffigen`. | One UI codebase |
| Video replay | `media_kit` for playback. The trajectory overlay is drawn with `CustomPainter`, synced to frame timestamps. | Interactive overlay, no re-encode |
| 3D replay | Our own small 3D renderer in `CustomPainter` (pitch, stumps, ball path, orbit/zoom gestures) | The scene is simple. No experimental 3D engine needed. Runs everywhere. |
| Storage | SQLite via `drift` (Android + Windows) | Overs, matches, player stats, saved locally |
| Wire protocol | TCP: length-prefixed H.264 Annex-B access units + capture timestamp per frame. Separate JSON-lines control channel for pairing, clock sync (NTP-style ping), result cards and timing. | Low latency, simple, accurate timestamps |
| Pairing | Laptop shows a QR code `{ip, port, token, name}`. The phone scans it (ZXing core, pure Java, works on API 21). Manual IP and UDP LAN discovery as fallbacks. | |
| Optional AI | **ONNX Runtime** as one API with execution providers: TensorRT/CUDA (RTX 4050), OpenVINO (Intel UHD), CPU. Auto-detected and always optional. The model runs on tiles/crops around Kalman predictions (the ball is tiny). | One integration, three backends |
| AI model licence | Avoid Ultralytics YOLOv8/11 (AGPL-3.0) unless you accept AGPL. Use **YOLOX-Nano (Apache-2.0)** or train our own tiny detector. **[DECIDE later]** | |
| Experiments | Python 3 + numpy + OpenCV: synthetic video renderer, evaluation scripts | |

### 3.1 Calibration **[DECIDE]**
- **Required:** tap the 4 pitch corners. Enter pitch length (stumps to stumps), pitch width, stump height **and stump width** (home stumps vary).
- **Strongly recommended:** also tap the stump base and top at both ends (4 more taps). The pitch is seen almost edge-on, so 4 corners alone estimate the focal length badly. With 8 points we solve focal length + pose (PnP with unknown f) robustly.
- Use the phone's reported FOV / focal length (Camera1 `getHorizontalViewAngle`, Camera2 `LENS_INFO_*` / `SENSOR_INFO_PHYSICAL_SIZE`) as a prior. A one-time checkerboard calibration is an optional "pro" step.
- Show the reprojection error after calibration, and warn if it is too high.

### 3.2 Modes & benchmark
- **camera_lite** (old phones): always phone + laptop mode.
- **Flutter app on Android 24+:** on first launch it runs the engine on a bundled synthetic 1080p clip and probes the camera (60 fps support?) and RAM. It then suggests phone-only or phone + laptop mode. The user can switch manually. The Flutter app can also act as the streaming camera for the laptop.

### 3.3 Latency budget & timing log
Every delivery JSON includes `timings_ms`: capture→send, network, decode, trigger, detect, track, fit, metrics, total (clock offset from the control-channel sync). Targets: ≤1 s after the ball reaches the batsman in live streaming mode, ≤2 s in clip mode. The classical engine on ~60–120 frames should take tens of ms on either laptop. Stream buffering and the network are the main risks, so they get logged.

**Encoder note (old phone):** a 4–5 px ball gets smeared by low-bitrate H.264. Start at **6–8 Mbps, Baseline profile, 1 s GOP, fps locked at 30**. That is well within practical 2.4 GHz throughput. We measure and tune from there.

---

## 4. Folder structure (proposed)

```
ball-tracking-home-match-/
├── README.md
├── docs/
│   ├── PLAN.md                 ← this file
│   ├── accuracy.md             measured accuracy per metric (generated from eval runs)
│   └── protocol.md             phone↔laptop wire + control protocol
├── engine/                     shared C++ tracking engine
│   ├── CMakeLists.txt
│   ├── cmake/                  toolchains (MSVC, Android NDK), deps
│   ├── include/pocketeye/      public C++ API + pocketeye_c.h (stable C ABI for FFI/JNI)
│   ├── src/
│   │   ├── calib/              homography, PnP w/ unknown focal, pitch model
│   │   ├── detect/             background model, blob/streak filter, (later) onnx detector
│   │   ├── track/              Kalman, gating, path selection
│   │   ├── events/             delivery trigger, bounce, bat/pad contact
│   │   ├── fit/                3D physics model, LM solver, covariance, Monte-Carlo
│   │   ├── metrics/            speed, swing, turn, line/length, wicket, shot power
│   │   ├── io/                 frame sources (FFmpeg file/stream), ring buffer
│   │   ├── host/               pe_host: TCP receiver + decode loop for laptop mode
│   │   └── util/               timing log, JSON, geometry
│   ├── cli/                    `pocketeye-cli`: video + calib.json → delivery JSON
│   └── tests/                  GoogleTest unit tests (engine math)
├── tools/                      Python
│   ├── synth/                  synthetic delivery renderer (known speed/swing/turn/bounce)
│   ├── eval/                   run CLI over datasets, compare with ground truth → docs/accuracy.md
│   └── requirements.txt
├── testdata/                   small clips + ground-truth JSON (Git LFS for video)
├── schemas/                    JSON Schemas: calibration, delivery result, result card
├── app/                        Flutter app (Windows + Android 24+)
│   ├── lib/
│   │   ├── engine/             ffigen bindings + Dart wrapper
│   │   ├── features/           calibration/ results/ replay/ replay3d/ scoring/ pairing/ settings/ benchmark/
│   │   ├── services/           stream server (laptop), storage (drift), clock sync
│   │   └── main.dart
│   ├── packages/pocketeye_camera/   Flutter plugin: Camera2 capture → engine via JNI (no frame copies through Dart)
│   ├── android/  windows/  test/
├── camera_lite/                native Kotlin app for Android 5.x phones (minSdk 21, no AndroidX)
└── .github/workflows/          CI: engine build + unit tests + synthetic accuracy gate
```

---

## 5. Build order & milestones (with exit criteria)

**Step 1: Engine + CLI (first, and the largest step)**
1a. `tools/synth`: render deliveries with a known camera pose, pitch size, ball type, speed, swing, bounce, turn and stump outcome. Includes motion blur, sensor noise, H.264 compression, frame-time jitter, VFR, and distractors (moving bowler/batsman blobs, shadows). Writes ground-truth JSON.
1b. Engine core: calibration → detection → tracking → events → 3D fit → metrics → JSON, plus a timing log.
1c. GoogleTest unit tests for the math: projection/PnP, physics fit on noiseless and noisy points, speed, swing, turn, bounce-point and wicket predicate, LM convergence, uncertainty coverage (are 90 % intervals right ~90 % of the time?).
1d. Accuracy gate: about 200 synthetic deliveries per preset (720p30, 1080p60). CI fails if errors exceed the documented bounds. This produces `docs/accuracy.md`.
1e. Real recordings: you record ~20 deliveries with the stock camera app, plus a reference where possible (a known bounce marker on the pitch, a radar gun or a speed app if available). We tune and report real accuracy.
*Exit:* synthetic errors within the table in §2.3 (or the table gets updated honestly), and real clips tracked end to end.

**Step 2: Windows laptop app.** Open a clip or folder → calibration screen → result cards → overlay replay. Uses the engine through FFI.

**Step 3: Old-phone side.** `camera_lite` (stream, QR pairing, result card), `pe_host` receiver, live delivery auto-detect, latency log. *Exit:* ≤1 s result latency on the SM-G531F + a laptop over home Wi-Fi.

**Step 4: Phone-only mode** on the Redmi Note 10S (Camera2 at 60 fps → engine on device) + the first-launch benchmark.

**Step 5: 3D replay + scoring** (runs, wickets, overs, extras, per-player stats, local DB).

**Step 6: Optional AI detector** (ONNX Runtime; TensorRT/CUDA, OpenVINO, CPU), auto-selected, compared against classical on the same eval set.

---

## 6. Risks & mitigations
| Risk | Mitigation |
|---|---|
| Ball only 4–5 px at the far end, blurred by H.264 | Higher bitrate, ROI-focused detection, streak model, physics prior bridging missed frames |
| Bowler hides the ball at release | Report release speed from the fitted model at the first reliable detection, and say so in the card |
| Too few frames after the bounce at 30 fps | Turn and wicket marked "low confidence" / "not enough data" instead of guessing |
| Old phone exposure → long streaks | Treat streaks as measurements (centre + direction); tips for brighter light |
| Calibration mistakes | Reprojection-error check, stump-tap step, visual overlay of the projected pitch for confirmation |
| Camera bumped mid-session | Detect drift (pitch-line alignment check) and prompt for recalibration |
| Wi-Fi hiccups | Timestamps make dropped frames harmless for timing; frame drops are logged |

---

## 7. Open questions for you
1. **Old phone:** OK to build the small native Kotlin `camera_lite` app (Option A), and use stock-camera recordings / IP Webcam only for early prototyping?
2. **Stack:** OK with C++17 + OpenCV + Eigen + FFmpeg for the engine, and Flutter + media_kit + drift + a hand-made 3D view for the app?
3. **Calibration:** OK to add the optional stump base/top taps (8 points total) and a stump-width field?
4. **Batsman handedness:** add a right/left-handed setting per batter (needed for off/leg and in/out-swing labels)?
5. **Name:** keep `pocketeye` or pick another?
6. **Licensing:** is the app for personal use only, or will it be published? This decides whether AGPL models (Ultralytics) are acceptable later.
7. **Real-test reference:** do you have any speed reference (radar gun, speed-gun app) for validating speed on real clips?
