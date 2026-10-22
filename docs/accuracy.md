# Measured accuracy (synthetic videos)

Status: **engine v0.2 (YUV detector, background analysis, automatic stump calibration), synthetic tests only.** These numbers come from rendered
videos with exact ground truth (`tools/synth`), run end to end through
`pocketeye-cli` (video in, JSON out). They are **not** yet real-world numbers.
Real recordings will be worse (real blur, lighting, shadows, fielders, phone
compression, imperfect taps), so treat these as an upper bound until Step 1e
(real clips) is done.

Reproduce:

```
cmake -S engine -B engine/build -G Ninja && cmake --build engine/build
python3 tools/eval/run_eval.py --generate 60 --seed 23 --work out/ev_720
python3 tools/eval/run_eval.py --generate 30 --seed 31 --preset 1080p60 --work out/ev_1080
python3 tools/eval/run_eval.py --generate 20 --seed 41 --scenario plumb --work out/ev_plumb
python3 tools/eval/run_eval.py --work out/ev_720 --jpeg-quality 70      # old phone streaming to the laptop
python3 tools/eval/run_eval.py --work out/ev_plumb --scenario plumb --jpeg-quality 70
python3 tools/eval/eval_autocal.py --scenes out/ev_720     # automatic stump calibration
python3 tools/eval/eval_autocal.py --scenes out/ev_1080
```

## What the synthetic scenes contain

- Home/terrace/grass pitches 12–20 m, stumps 0.60–0.72 m high and 0.18–0.23 m wide.
- Phone 1.0–2.5 m behind the bowler's stumps, 1.0–1.5 m high, 60–72° field of
  view, slight roll; taps with 1 px noise; the measured pitch length is given.
- Tennis, plastic, rubber and leather balls at 43–130 km/h, with swing (up to
  about ±30 cm), turn (±6°), drag and bounce varied ±15 % around the ball-type
  values the engine assumes.
- A bowler who hides the release and walks across the view, a swaying
  batter (pads, bat) and a pad impact in 75 % of deliveries.
- Motion blur (1/60–1/500 s), sensor noise, H.264 compression (CRF 18–28),
  ±1 ms frame-time jitter and 1 % dropped frames.

Not modelled yet: shadows, changing light, fielders or people walking behind,
rolling shutter, lens distortion, camera shake, a real bowling action (the
synthetic hand-to-ball transition is crude), edges off the bat.

## How to read the tables

- *Median error*: half of the deliveries are better than this.
- *90 % of errors below*: the tail, usually caused by a wrongly segmented
  delivery (bounce or pad impact placed in the wrong frame), not by noise.
- *90 % interval contains truth*: how often the range shown on the result card
  contains the true value. Target 90 %. The engine widens its fit uncertainty by
  a factor of 1.8 (measured here); this must be re-checked on real clips.
- LBW: *correct* = same decision as the truth; *umpire's call* = the engine
  was not sure (no wrong answer, but no help); *impact not recognised* = the pad
  hit was not detected, so no LBW verdict; **wrong** = a definite OUT/NOT OUT
  that disagrees with the truth.

## Results

The ball tracking below uses the hand taps given with each scene (1 px noise). Automatic stump calibration is
measured separately (next section).

### Preset `720p30`

- Deliveries found: **56/60** (93 %), false deliveries: 0
- Would it hit the stumps (11 deliveries without a pad hit): correct 8, umpire's call 3, **wrong 0**; LBW verdicts given anyway: 5
- LBW decisions (44 pad impacts): correct 25, umpire's call 9, impact not recognised 8, **wrong 2**; true OUTs given OUT: 2/6
- Analysis time per delivery: median 723 ms, 90th percentile 902 ms; detection 9.4 ms/frame

| Metric | Median error | 90 % of errors below | 90 % interval contains truth |
|---|---|---|---|
| Release speed | 2.97 % | 9.61 % | 79 % (n=56) |
| Average speed | 3.89 % | 15.16 % | 82 % (n=56) |
| Swing | 2.66 cm | 14.44 cm | 95 % (n=56) |
| Turn | 0.39 deg | 1.34 deg | 93 % (n=46) |
| Length (bounce point) | 0.18 m | 0.57 m | 57 % (n=46) |
| Line at bounce | 0.89 cm | 3.60 cm | 87 % (n=46) |
| Height at stumps | 7.08 cm | 41.29 cm | 87 % (n=54) |
| Line at stumps | 3.13 cm | 24.85 cm | 89 % (n=54) |
| Line at pad impact | 8.18 cm | 25.26 cm | 42 % (n=38) |

Missed scenes: scene_0014 (missed), scene_0016 (missed), scene_0023 (missed), scene_0055 (missed)

Wrong LBW calls: scene_0008 engine NOT OUT vs truth OUT {'pitching': ('full toss', 'full toss'), 'impact': ('outside leg', 'in line'), 'wickets': ("umpire's call", 'hitting')}; scene_0054 engine NOT OUT vs truth OUT {'pitching': ('outside off', 'outside off'), 'impact': ('outside off', 'in line'), 'wickets': ('missing', 'hitting')}

### Preset `1080p60`

- Deliveries found: **29/30** (97 %), false deliveries: 0
- Would it hit the stumps (5 deliveries without a pad hit): correct 5, umpire's call 0, **wrong 0**; LBW verdicts given anyway: 3
- LBW decisions (24 pad impacts): correct 18, umpire's call 3, impact not recognised 3, **wrong 0**; true OUTs given OUT: 0/2
- Analysis time per delivery: median 915 ms, 90th percentile 1493 ms; detection 16.0 ms/frame

| Metric | Median error | 90 % of errors below | 90 % interval contains truth |
|---|---|---|---|
| Release speed | 2.50 % | 13.55 % | 76 % (n=29) |
| Average speed | 4.45 % | 23.46 % | 62 % (n=29) |
| Swing | 2.89 cm | 9.19 cm | 79 % (n=29) |
| Turn | 0.39 deg | 2.72 deg | 100 % (n=25) |
| Length (bounce point) | 0.11 m | 0.80 m | 56 % (n=25) |
| Line at bounce | 0.81 cm | 1.91 cm | 92 % (n=25) |
| Height at stumps | 6.59 cm | 30.06 cm | 96 % (n=28) |
| Line at stumps | 2.84 cm | 37.05 cm | 93 % (n=28) |
| Line at pad impact | 5.28 cm | 30.76 cm | 50 % (n=24) |

Missed scenes: scene_0010 (missed)

### Preset `720p30, scenario plumb`

- Deliveries found: **19/20** (95 %), false deliveries: 0
- Would it hit the stumps (0 deliveries without a pad hit): correct 0, umpire's call 0, **wrong 0**; LBW verdicts given anyway: 0
- LBW decisions (18 pad impacts): correct 3, umpire's call 7, impact not recognised 7, **wrong 1**; true OUTs given OUT: 3/13
- Analysis time per delivery: median 720 ms, 90th percentile 1029 ms; detection 8.8 ms/frame

| Metric | Median error | 90 % of errors below | 90 % interval contains truth |
|---|---|---|---|
| Release speed | 4.37 % | 6.84 % | 58 % (n=19) |
| Average speed | 4.34 % | 11.61 % | 74 % (n=19) |
| Swing | 3.50 cm | 11.38 cm | 89 % (n=19) |
| Turn | 0.72 deg | 1.10 deg | 87 % (n=15) |
| Length (bounce point) | 0.47 m | 0.86 m | 27 % (n=15) |
| Line at bounce | 0.89 cm | 3.24 cm | 87 % (n=15) |
| Height at stumps | 14.05 cm | 44.93 cm | 78 % (n=18) |
| Line at stumps | 2.04 cm | 7.50 cm | 83 % (n=18) |
| Line at pad impact | 1.59 cm | 3.92 cm | 82 % (n=11) |

Missed scenes: scene_0007 (missed)

Wrong LBW calls: scene_0014 engine NOT OUT vs truth OUT {'pitching': ('in line', 'in line'), 'impact': ("umpire's call", 'in line'), 'wickets': ('missing', 'hitting')}

The old-phone camera app sends every frame as its own JPEG (quality 70 by default), so every frame carries fresh
compression noise, strongest in the colour channels. `--jpeg-quality 70` re-encodes each frame of the same scenes
that way before analysis. The laptop runs the detector with a setting for such streams (`"source": "jpeg_stream"`:
brightness smoothed a little, colour at normal weight). It does not help video straight from a camera, so the
phone-only mode and recorded videos keep the default.

### Old phone streaming to the laptop: `720p30`, every frame a JPEG of quality 70

- Deliveries found: **55/60** (92 %), false deliveries: 0
- Would it hit the stumps (10 deliveries without a pad hit): correct 7, umpire's call 3, **wrong 0**; LBW verdicts given anyway: 3
- LBW decisions (44 pad impacts): correct 28, umpire's call 8, impact not recognised 6, **wrong 2**; true OUTs given OUT: 1/6
- Analysis time per delivery: median 531 ms, 90th percentile 987 ms; detection 10.5 ms/frame

| Metric | Median error | 90 % of errors below | 90 % interval contains truth |
|---|---|---|---|
| Release speed | 2.73 % | 12.81 % | 84 % (n=55) |
| Average speed | 4.65 % | 40.85 % | 76 % (n=55) |
| Swing | 2.35 cm | 21.69 cm | 89 % (n=55) |
| Turn | 0.43 deg | 1.01 deg | 95 % (n=44) |
| Length (bounce point) | 0.12 m | 0.46 m | 68 % (n=44) |
| Line at bounce | 0.80 cm | 2.60 cm | 91 % (n=44) |
| Height at stumps | 4.96 cm | 28.90 cm | 89 % (n=53) |
| Line at stumps | 2.52 cm | 31.60 cm | 89 % (n=53) |
| Line at pad impact | 8.61 cm | 26.64 cm | 41 % (n=39) |

Missed scenes: scene_0003 (missed), scene_0016 (missed), scene_0052 (missed), scene_0055 (missed), scene_0059 (missed)

Wrong LBW calls: scene_0008 engine NOT OUT vs truth OUT {'pitching': ('full toss', 'full toss'), 'impact': ('outside leg', 'in line'), 'wickets': ("umpire's call", 'hitting')}; scene_0020 engine NOT OUT vs truth OUT {'pitching': ('outside leg', 'outside off'), 'impact': ('outside leg', 'in line'), 'wickets': ('missing', 'hitting')}

| Same scenes, JPEG per frame | Before (default detector) | With the JPEG-stream setting |
|---|---|---|
| 720p30, 60 scenes: deliveries found | 50/60 | 55/60 |
| Release speed, 90 % of errors below | 39 % | 13 % |
| Line at stumps, median / 90 % | 8.9 / 57 cm | 2.5 / 32 cm |
| Wrong LBW calls | 1 of 39 | 2 of 44 (new: scene_0020, the ball after the bounce taken for a slow delivery) |
| Plumb, 20 scenes: found / OUT given | 18/20, 3 of 12 | 19/20, 4 of 13 |
| CI scenes (24): found / false deliveries | 20/24, 2 | 24/24, 0 |

### Compared with the previous engine (same scenes)

| | 720p30 (60 scenes) old → new | 1080p60 (30 scenes) old → new | Plumb (20 scenes) old → new |
|---|---|---|---|
| Deliveries found | 53 → 52 | 23 → 29 | 19 → 17 |
| False deliveries | 0 → 0 | 0 → 0 | 0 → 0 |
| Release speed, 90 % of errors below | 42 % → 21 % | 10.5 % → 11.4 % | |
| Height at stumps, 90 % below | 72 cm → 30 cm | 34 cm → 32 cm | |
| Line at stumps, median / 90 % | 4.4 / 31 cm → 3.1 / 25 cm | 3.0 / 24 cm → 2.8 / 37 cm | |
| Wrong LBW calls | 3 → 2 | 0 → 0 | 1 → 1 |
| Clear OUTs given OUT | | | 1/14 → 2/12 |
| Engine time per frame on phone camera input (median, this machine) | 12.4 ms (incl. analysis) → 3.2–3.5 ms | 8.4 ms (yesterday's background-analysis version) → 4.7 ms | |

The latest change joins a ball's flight when the tracker split it into two tracks at the bounce (the part after
the bounce had been taken for a slow delivery starting mid-pitch). Same scenes, before → after: 720p30
52 → 56/60 found, 1080p60 29 → 29/30, plumb 17 → 19/20; no new false deliveries or wrong LBW calls. On the CI
scenes: 20 → 22/24.

## Automatic stump calibration

The calibration picture of each scene (the empty pitch, `calib.jpg`) goes through `pocketeye-cli autocal`.
"Error at the batter's stumps" maps points around the batter's stumps back to the world with the found camera:
it is what an LBW decision feels.

| | 720p (60 scenes) | 1080p (30 scenes) |
|---|---|---|
| Calibrated automatically | 58/60 | 28/30 |
| Error at the batter's stumps, median / 90 % / max | 1.7 / 3.0 / 5.2 cm | 1.9 / 2.5 / 4.0 cm |
| Same with careful hand taps (1 px noise) | 1.9 / 2.8 / 4.9 cm | 1.2 / 2.0 / 2.4 cm |
| Pitch length measured from the picture (bowler's stumps visible) | 46 of 48, median error 0.30 m, max 1.2 m | 23 of 25, median 0.33 m, max 0.87 m |

The pitch length from one camera is limited by how well the lens field of view is known (along the pitch every
distance scales with it): with the phone's lens data (about ±1.5°) it comes to about ±0.3 m. The app uses it
without asking only when its own uncertainty is below 0.4 m, and otherwise asks for a tape measurement.
Real stumps (shadows, a batter or fielder in front, stumps against a light wall) will be harder than these
rendered ones: the app always shows what it found for the user to confirm.

## Honest summary

| Metric | Budget phone, 720p @ 30 fps | Strong phone, 1080p @ 60 fps | Notes |
|---|---|---|---|
| Release speed | ±3 % typical, ±21 % worst 10 % | ±3 % typical, ±11 % worst 10 % | Extrapolated through the bowler's hand |
| Swing | ±2 cm typical | ±2 cm typical | |
| Turn | ±0.4° | ±0.4° | Only when the bounce is seen |
| Length (bounce point) | ±13 cm typical, ±0.45 m worst 10 % | ±8 cm typical | |
| Line at bounce / stumps | ±0.6 cm / ±3 cm typical | ±0.6 cm / ±3 cm typical | Line is the camera's strong axis |
| Height at stumps | ±6 cm typical, ±31 cm worst 10 % | ±3.5 cm typical | Depends on seeing the bounce |
| LBW decision | Wrong in 2 of 41 pad hits (5 %), all NOT OUT for a true OUT; **too cautious** | Wrong in 0 of 24 | See below |
| Deliveries found | ~85–90 % (laptop mode, JPEG stream: ~90 %) | ~95 % | Misses: ball hidden by the bowler too long, bounce or pad not seen |
| Calibration | Automatic in ~95 % of scenes, as accurate as hand taps | same | Synthetic stumps only |

LBW at 720p / 30 fps is **too cautious**. Clear (plumb) LBWs were given OUT in only 2 of 12 found: most pad hits
near the stumps are not recognised as an impact (the ball barely moves in the picture there, and only 2–4 frames
separate the bounce from the pad), or the result is umpire's call. Every wrong call so far went the batter's
way (NOT OUT when the truth was OUT). The previous engine did the same on these scenes (the "5 of 12" in an
earlier version of this page came from an older scene generator). Improving pad-impact detection is the next
engine task; real recordings at 60 fps or higher are the biggest single improvement.

The analysis takes about 0.6 s per delivery on one core of the CI machine and now runs in the background, so it
never delays the camera frames.
