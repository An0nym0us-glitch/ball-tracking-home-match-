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

- Deliveries found: **54/60** (90 %), false deliveries: 0
- LBW decisions (42 pad impacts): correct 18, umpire's call 11, impact not recognised 10, **wrong 3**; true OUTs given OUT: 1/6
- Analysis time per delivery: median 605 ms, 90th percentile 926 ms; detection 7.9 ms/frame

| Metric | Median error | 90 % of errors below | 90 % interval contains truth |
|---|---|---|---|
| Release speed | 3.19 % | 21.25 % | 74 % (n=54) |
| Average speed | 4.60 % | 33.96 % | 81 % (n=54) |
| Swing | 2.50 cm | 31.95 cm | 89 % (n=54) |
| Turn | 0.43 deg | 1.28 deg | 98 % (n=40) |
| Length (bounce point) | 0.15 m | 0.37 m | 62 % (n=40) |
| Line at bounce | 0.95 cm | 3.26 cm | 85 % (n=40) |
| Height at stumps | 6.53 cm | 30.01 cm | 81 % (n=53) |
| Line at stumps | 3.70 cm | 19.01 cm | 87 % (n=53) |
| Line at pad impact | 8.04 cm | 21.19 cm | 43 % (n=35) |

Missed scenes: scene_0001 (missed), scene_0007 (missed), scene_0023 (missed), scene_0032 (missed), scene_0037 (missed), scene_0055 (missed)

Wrong LBW calls: scene_0008 engine NOT OUT vs truth OUT {'pitching': ('full toss', 'full toss'), 'impact': ('outside leg', 'in line'), 'wickets': ("umpire's call", 'hitting')}; scene_0020 engine NOT OUT vs truth OUT {'pitching': ('outside leg', 'outside off'), 'impact': ('outside leg', 'in line'), 'wickets': ('missing', 'hitting')}; scene_0054 engine NOT OUT vs truth OUT {'pitching': ('full toss', 'outside off'), 'impact': ('outside off', 'in line'), 'wickets': ('missing', 'hitting')}

### Preset `720p30`

- Deliveries found: **29/30** (97 %), false deliveries: 0
- LBW decisions (24 pad impacts): correct 20, umpire's call 3, impact not recognised 1, **wrong 0**; true OUTs given OUT: 0/2
- Analysis time per delivery: median 864 ms, 90th percentile 1652 ms; detection 13.2 ms/frame

| Metric | Median error | 90 % of errors below | 90 % interval contains truth |
|---|---|---|---|
| Release speed | 2.64 % | 11.40 % | 76 % (n=29) |
| Average speed | 4.45 % | 23.31 % | 62 % (n=29) |
| Swing | 2.99 cm | 9.36 cm | 83 % (n=29) |
| Turn | 0.56 deg | 3.80 deg | 96 % (n=26) |
| Length (bounce point) | 0.10 m | 0.99 m | 62 % (n=26) |
| Line at bounce | 0.58 cm | 1.91 cm | 96 % (n=26) |
| Height at stumps | 3.88 cm | 31.76 cm | 96 % (n=28) |
| Line at stumps | 2.63 cm | 38.35 cm | 100 % (n=28) |
| Line at pad impact | 4.03 cm | 30.96 cm | 54 % (n=24) |

Missed scenes: scene_0010 (missed)

### Preset `720p30, scenario plumb`

- Deliveries found: **17/20** (85 %), false deliveries: 0
- LBW decisions (16 pad impacts): correct 2, umpire's call 6, impact not recognised 7, **wrong 1**; true OUTs given OUT: 2/12
- Analysis time per delivery: median 730 ms, 90th percentile 945 ms; detection 7.5 ms/frame

| Metric | Median error | 90 % of errors below | 90 % interval contains truth |
|---|---|---|---|
| Release speed | 4.94 % | 10.20 % | 53 % (n=17) |
| Average speed | 4.27 % | 10.65 % | 71 % (n=17) |
| Swing | 3.35 cm | 12.38 cm | 88 % (n=17) |
| Turn | 0.66 deg | 1.63 deg | 86 % (n=14) |
| Length (bounce point) | 0.37 m | 0.77 m | 36 % (n=14) |
| Line at bounce | 0.94 cm | 1.90 cm | 93 % (n=14) |
| Height at stumps | 8.00 cm | 22.22 cm | 88 % (n=16) |
| Line at stumps | 1.50 cm | 6.59 cm | 94 % (n=16) |
| Line at pad impact | 1.34 cm | 3.00 cm | 91 % (n=11) |

Missed scenes: scene_0001 (missed), scene_0002 (missed), scene_0007 (missed)

Wrong LBW calls: scene_0014 engine NOT OUT vs truth OUT {'pitching': ('in line', 'in line'), 'impact': ("umpire's call", 'in line'), 'wickets': ('missing', 'hitting')}

### Compared with the previous engine (same scenes)

| | 720p30 (60 scenes) old → new | 1080p60 (30 scenes) old → new | Plumb (20 scenes) old → new |
|---|---|---|---|
| Deliveries found | 53 → 54 | 23 → 29 | 19 → 17 |
| False deliveries | 0 → 0 | 0 → 0 | 0 → 0 |
| Release speed, 90 % of errors below | 42 % → 21 % | 10.5 % → 11.4 % | |
| Height at stumps, 90 % below | 72 cm → 30 cm | 34 cm → 32 cm | |
| Line at stumps, median / 90 % | 4.4 / 31 cm → 3.7 / 19 cm | 3.0 / 24 cm → 2.6 / 38 cm | |
| Wrong LBW calls | 3 → 3 | 0 → 0 | 1 → 1 |
| Clear OUTs given OUT | | | 1/14 → 2/12 |
| Engine time per frame on phone camera input (median, this machine) | 12.4 ms (incl. analysis) → 3.2–3.5 ms | 8.4 ms (yesterday's background-analysis version) → 4.7 ms | |

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
| LBW decision | Wrong in 3 of 42 pad hits (7 %), all NOT OUT for a true OUT; **too cautious** | Wrong in 0 of 24 | See below |
| Deliveries found | ~85–90 % | ~95 % | Misses: ball hidden by the bowler too long, bounce or pad not seen |
| Calibration | Automatic in ~95 % of scenes, as accurate as hand taps | same | Synthetic stumps only |

LBW at 720p / 30 fps is **too cautious**. Clear (plumb) LBWs were given OUT in only 2 of 12 found: most pad hits
near the stumps are not recognised as an impact (the ball barely moves in the picture there, and only 2–4 frames
separate the bounce from the pad), or the result is umpire's call. Every wrong call so far went the batter's
way (NOT OUT when the truth was OUT). The previous engine did the same on these scenes (the "5 of 12" in an
earlier version of this page came from an older scene generator). Improving pad-impact detection is the next
engine task; real recordings at 60 fps or higher are the biggest single improvement.

The analysis takes about 0.6 s per delivery on one core of the CI machine and now runs in the background, so it
never delays the camera frames.
