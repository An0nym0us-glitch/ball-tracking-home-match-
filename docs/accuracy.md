# Measured accuracy (synthetic videos)

Status: **engine v0.1, synthetic tests only.** These numbers come from rendered
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

### Preset `720p30`

- Deliveries found: **49/60** (82 %), false deliveries: 0
- LBW decisions (36 pad impacts): correct 20, umpire's call 9, impact not recognised 6, **wrong 1**; true OUTs given OUT: 0/7
- Analysis time per delivery: median 510 ms, 90th percentile 723 ms; detection 6.0 ms/frame

| Metric | Median error | 90 % of errors below | 90 % interval contains truth |
|---|---|---|---|
| Release speed | 4.12 % | 44.10 % | 78 % (n=49) |
| Average speed | 6.75 % | 38.42 % | 65 % (n=49) |
| Swing | 3.26 cm | 69.40 cm | 86 % (n=49) |
| Turn | 0.60 deg | 1.82 deg | 100 % (n=40) |
| Length (bounce point) | 0.09 m | 0.56 m | 80 % (n=40) |
| Line at bounce | 0.59 cm | 5.72 cm | 80 % (n=40) |
| Height at stumps | 6.15 cm | 55.27 cm | 85 % (n=48) |
| Line at stumps | 3.04 cm | 50.24 cm | 88 % (n=48) |
| Line at pad impact | 6.28 cm | 32.21 cm | 44 % (n=32) |


Wrong LBW calls: scene_0042 engine NOT OUT vs truth OUT {'pitching': ('outside off', 'outside off'), 'impact': ('outside off', 'in line'), 'wickets': ('missing', 'hitting')}

### Preset `1080p60`

- Deliveries found: **24/30** (80 %), false deliveries: 0
- LBW decisions (18 pad impacts): correct 15, umpire's call 3, impact not recognised 0, **wrong 0**; true OUTs given OUT: 1/2
- Analysis time per delivery: median 434 ms, 90th percentile 1454 ms; detection 13.4 ms/frame

| Metric | Median error | 90 % of errors below | 90 % interval contains truth |
|---|---|---|---|
| Release speed | 1.16 % | 4.77 % | 96 % (n=24) |
| Average speed | 3.55 % | 13.95 % | 79 % (n=24) |
| Swing | 2.05 cm | 7.31 cm | 83 % (n=24) |
| Turn | 0.43 deg | 0.95 deg | 85 % (n=20) |
| Length (bounce point) | 0.06 m | 0.26 m | 75 % (n=20) |
| Line at bounce | 0.67 cm | 1.55 cm | 90 % (n=20) |
| Height at stumps | 3.18 cm | 24.13 cm | 100 % (n=24) |
| Line at stumps | 1.41 cm | 8.38 cm | 100 % (n=24) |
| Line at pad impact | 4.39 cm | 16.50 cm | 44 % (n=18) |

### Preset `720p30, scenario plumb`

- Deliveries found: **18/20** (90 %), false deliveries: 0
- LBW decisions (18 pad impacts): correct 8, umpire's call 4, impact not recognised 5, **wrong 1**; true OUTs given OUT: 5/12
- Analysis time per delivery: median 589 ms, 90th percentile 894 ms; detection 5.7 ms/frame

| Metric | Median error | 90 % of errors below | 90 % interval contains truth |
|---|---|---|---|
| Release speed | 2.98 % | 6.12 % | 83 % (n=18) |
| Average speed | 3.26 % | 8.56 % | 78 % (n=18) |
| Swing | 2.27 cm | 11.74 cm | 83 % (n=18) |
| Turn | 0.44 deg | 1.98 deg | 86 % (n=14) |
| Length (bounce point) | 0.31 m | 0.71 m | 50 % (n=14) |
| Line at bounce | 1.72 cm | 3.45 cm | 86 % (n=14) |
| Height at stumps | 10.28 cm | 55.81 cm | 78 % (n=18) |
| Line at stumps | 2.03 cm | 15.05 cm | 89 % (n=18) |
| Line at pad impact | 2.23 cm | 7.04 cm | 93 % (n=15) |


Wrong LBW calls: scene_0004 engine NOT OUT vs truth OUT {'pitching': ('in line', 'in line'), 'impact': ('outside off', 'in line'), 'wickets': ('missing', 'hitting')}

## Honest summary

| Metric | Budget phone, 720p @ 30 fps | Strong phone, 1080p @ 60 fps | Notes |
|---|---|---|---|
| Release speed | ±4 % typical, occasional big misses | ±1 % typical, ±5 % worst 10 % | Extrapolated through the bowler's hand |
| Swing | ±3 cm typical | ±2 cm typical | |
| Turn | ±0.6° | ±0.4° | Only when the bounce is seen |
| Length (bounce point) | ±10 cm typical, ±0.6 m worst 10 % | ±6 cm typical | |
| Line at bounce / stumps | ±1 cm / ±3 cm typical | ±1 cm / ±1.5 cm typical | Line is the camera's strong axis |
| Height at stumps | ±6 cm typical, ±55 cm worst 10 % | ±3 cm typical | Depends on seeing the bounce |
| Pad impact position | ±6 cm typical | ±4 cm typical | Impact time is only known to a frame |
| LBW decision | Wrong in 1 of 36 pad hits (3 %); **too cautious**: many umpire's calls and missed impacts | Wrong in 0 of 18 pad hits | See below |
| Deliveries found | ~80–90 % | ~80 % | Misses: ball hidden by the bowler for too long, bounce or pad not seen |

LBW at 720p / 30 fps is **safe but too cautious** right now. Clear (plumb) LBWs
were given OUT in only 5 of 12 cases. The rest were umpire's call, or the pad
impact was not recognised (the ball barely moves in the image near the batter,
and only 2–4 frames separate a full-length bounce from the pad). The two wrong calls
(in 54 pad hits over both 720p sets) came from a bounce right in front of the pad
being missed.
Planned work: better pad-impact detection, using colour and size changes after
the impact, and a 60 fps capture mode wherever the phone supports it.

The analysis takes about 0.5 s per delivery on one laptop core (CI machine),
and detection about 6 ms per 720p frame, so it is well within the 1–2 s target.
Measure on the target laptops in Step 2.
