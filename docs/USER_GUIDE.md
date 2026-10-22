# PocketEye user guide

PocketEye gives LBW decisions from one phone standing behind the bowler's stumps.
There are three apps:

| App | Runs on | What it does |
|---|---|---|
| **PocketEye** (Windows) | Laptop, Windows 10/11, 64-bit | Live mode: receives the phone camera over Wi-Fi and gives the decision. File mode: analyses a recorded video. |
| **PocketEye Camera** | Old phones, Android 5 or newer (e.g. Galaxy Grand Prime SM-G531F) | Only a camera: streams the video to the laptop and shows the result card it gets back. |
| **PocketEye** (Android) | Newer phones, Android 7 or newer (e.g. Redmi Note 10S) | Phone-only mode: does all the tracking on the phone, no laptop needed. |

## 1. Download

The apps are built automatically on GitHub for every change.

Easiest, including straight from the phone: open the repository on GitHub → **Releases** → the newest
**PocketEye build** (marked *Latest*) and tap the file you need. These links always give the newest build:

- https://github.com/An0nym0us-glitch/ball-tracking-home-match-/releases/latest/download/PocketEye-arm64.apk
- https://github.com/An0nym0us-glitch/ball-tracking-home-match-/releases/latest/download/PocketEyeCamera.apk
- https://github.com/An0nym0us-glitch/ball-tracking-home-match-/releases/latest/download/PocketEye-windows.zip

Or, signed in to GitHub:

1. Open the repository on GitHub → **Actions** → workflow **apps** → the newest green run on the branch.
2. At the bottom, under **Artifacts**, download:
   - `PocketEye-windows`: contains `PocketEye-windows.zip` (the laptop app) and `pocketeye-cli-windows.zip` (command-line tool).
   - `PocketEye-android`: contains `PocketEye-arm64.apk`, `PocketEye-arm32.apk` and `PocketEyeCamera.apk`.

Which APK?

- Old phone that streams to the laptop → `PocketEyeCamera.apk`.
- Phone-only mode → `PocketEye-arm64.apk`. If it says "app not installed" or the phone is 32-bit, use `PocketEye-arm32.apk`.

Install an APK: copy it to the phone, open it, and allow "install from unknown sources" when asked.

Laptop: unzip `PocketEye-windows.zip` anywhere and run `PocketEye.exe`. Keep all the files next to it.
On first start, Windows may show "Windows protected your PC": press **More info → Run anyway** (the app is not signed).
When Windows asks about network access, allow **private networks**. The phone cannot connect otherwise.

## 2. Put the phone in place

- On a stand or tripod **behind the bowler's stumps**, about 1–1.5 m high, looking straight down the pitch at the batter.
- The batter's stumps must be fully visible, top to bottom. The bowler's stumps help but are optional.
- Landscape, steady, and it must not move after calibration. Re-calibrate if it is bumped.
- Good light. Avoid pointing the camera into the sun.

## 3. Pitch and match setup (once)

In the app, open **Pitch and match setup** and enter:

- the stump height (ground to the top of the stumps) and width (outer edge to outer edge), measured with a tape;
- the ball type (tennis/tape, plastic, rubber, leather) and the batter: right-handed, left-handed, or
  **No batter** for bowling practice at empty stumps (then there is no LBW; each ball shows its speed, line,
  length and whether it would hit the stumps, or "BOWLED" when it hits them). Choose right/left-handed only when
  someone is batting: with nobody there, a ball that hits the stumps can come out as an LBW decision (NOT OUT
  or UMPIRE'S CALL), because near the stumps the ball is often lost from view or its deflection is taken for a
  pad. The live screens have a quick **Batter: Right / Left / Nobody (practice)** switch;
- **Bolder decisions** (off by default): a definite OUT / NOT OUT when the app is 80 % sure instead of 90 %.
  Fewer UMPIRE'S CALLs, but a few more wrong decisions (see [accuracy.md](accuracy.md));
- the pitch length (stumps to stumps), **or leave it empty**: the app then measures it from the picture.
  This needs the bowler's stumps in the picture. A tape measure is still more exact (the picture gives
  about ±0.3 m), and the app asks for it when the picture is not exact enough.

If the bowler's stumps are not in the picture, the app also needs the pitch length, the camera height and its
distance behind the bowler's stumps.

**Stumps only at the batter's end** (the bowler bowls from a mark): switch off **Stumps at the bowler's end
too**. Then measure the pitch length from the bowling mark to the batter's stumps, the camera height, and how far
the camera is behind the bowling mark. The app only looks for the batter's stumps when calibrating.

## 4. Calibrate (the app finds the stumps)

Calibrate **before the batter takes guard**, so that nothing stands in front of the stumps.
Every mode opens the same calibration screen on a still picture from the camera:

1. The app looks for both sets of stumps by itself (1–3 s) and draws yellow lines on them and along the pitch.
2. Check that the yellow lines sit on the real stumps. If they do, press **Save calibration**.
3. If they don't, press **Tap them myself** and tap the points it asks for: bottom and top of the **left** and
   **right** outer stump at the batter's end ("left/right" as seen in the picture), then the bowler's stumps if
   visible (or **Skip (not visible)**), then **Check**. **Find automatically** tries again.
4. If the app asks for the pitch length, measure it and press **Use**.

## 5a. Phone + laptop (old phones)

1. Phone and laptop on the **same Wi-Fi** (a phone hotspot that the laptop joins works too).
2. Laptop: **Live: phone camera to this laptop**. It shows a QR code and a 6-digit code.
3. Phone: open **PocketEye Camera** → **Scan laptop QR** and point it at the screen. If scanning fails, use **Type address** and enter the address, port and code shown under the QR code.
4. Put the phone on its stand. On the laptop press **Calibrate** (the app finds the stumps; you check).
5. Bowl. Each delivery appears on the laptop as a card, and the same result card shows on the phone a moment later.
6. Click a card for the replay (slow motion, **Zoom on batter**), the pitch map and all measurements.

While tracking, the laptop shows the phone's picture live with a **red box that follows the ball** and an orange
trail behind it, like a phone camera following a face. The box appears a few frames after the ball comes into view
and stays on it through the bounce. It only starts on something heading for the batter's stumps, so the
bowler's arm swinging up and round is mostly ignored (it can still sit on the hand for a moment at the release). If the box never appears on a thrown ball, the ball is not being seen: switch on
**Show what the engine sees** (green: area watched for the ball; yellow: everything that could be the ball in the
last second) and check the light and the calibration. Under the picture, "Balls found" and "other movement ignored
(last: ...)" say what the engine made of each movement.

After calibrating, the laptop tells the phone to fix its brightness and focus on the batter's stumps (otherwise the
picture changes every time the bowler runs past the camera, which hides the ball). The phone shows "Brightness and
focus locked on the stumps". If the light changes a lot, calibrate again.

**Quality** on the phone sets the picture size and JPEG quality: 1280×720 is the most accurate, and 960×540 or 640×480 helps on weak Wi-Fi. A higher JPEG quality (80) gives the laptop a cleaner picture if the Wi-Fi keeps up (watch "Frames" on the laptop). After changing the size, calibrate again.

## 5b. Phone-only (newer phones)

On first launch the app runs a short speed test and suggests a mode:

- "phone-only 1080p 60 fps" or "phone-only 30 fps": this phone is fast enough;
- "laptop": this phone is too slow, so use the phone + laptop mode.

**Re-test** repeats it. Then:

1. **Phone-only mode** → put the phone on the stand → **Calibrate** (the app finds the stumps; you check).
2. Bowl. Cards appear on the phone, and tapping one opens the replay.

Camera size: **1080p** (default, sharper) or **720p**. The app asks the camera for 60 fps; if the phone cannot do
60 fps at 1080p it falls back to 720p 60 fps, then 30 fps. The screen shows what the camera really delivers
(for example "1920x1080 at 60 fps"). Changing the size needs a new calibration.

Hold the phone in **landscape** (the app keeps the screen in landscape in this mode).

**Balls not found?** Switch on **Show what the engine sees**. It shows the last picture with the area watched
for the ball (green) and every spot that could be the ball in the last second (yellow circles). A thrown ball
should leave a line of yellow circles along the pitch. If it does not, press **Record test clip**, bowl a ball
within 8 seconds and send the clip (the share menu opens: WhatsApp, Drive, e-mail, ...). The clip is exactly
what the engine received, so the problem can be found and fixed.

After calibrating, the app fixes the camera's brightness and focus on the batter's stumps (otherwise the picture
changes every time the bowler runs past, which hides the ball). If the light changes a lot, calibrate again.

The screen shows the engine time per frame, how many frames were skipped, how many balls were found and how many
other moving things were ignored (with the last reason). If many frames are skipped, accuracy drops. Use the laptop
mode for important games.

## 6. Recorded videos (laptop)

**Analyse a recorded video** → **Choose video** → **Calibrate** → **Analyse**.
Record with the phone's normal camera app at the highest frame rate it offers (60 fps is much better than 30).
**Open replay video** shows the clip with the path drawn on it.

## 7. Reading the result

- **LBW: OUT / NOT OUT / UMPIRE'S CALL**, with pitching (in line / outside off / outside leg), impact (in line / outside) and wickets (hitting / missing / umpire's call).
- Tick **No shot** on a card when the batter played no shot. An impact outside off stump can then still be out.
- Speed, swing, turn, length and bounce height each come with a 90 % range in brackets. A wide range means "not sure".

## 8. Honest limits

- Tested on computer-made (synthetic) videos only, not yet on real phones or real cricket. See [accuracy.md](accuracy.md).
- At 30 fps (old phones) the ball moves 50–100 cm between frames. The app is cautious, so clear LBWs often come out as **UMPIRE'S CALL**. 60 fps helps a lot.
- One camera cannot see depth directly. Length and bounce come from the stump calibration, so careful taps and measurements matter.
- The ball must be visible against the background. Fast balls in poor light may not be found.

## Problems

| Problem | Try |
|---|---|
| Phone says "Laptop not found" | Same Wi-Fi? Live screen open on the laptop? Windows firewall allowed for private networks? Some public or office Wi-Fi blocks devices from seeing each other: use a phone hotspot. |
| "Camera size changed: calibrate again" | The phone picture size changed (Quality setting). Calibrate again. |
| No deliveries found | Check the calibration lines, the light and that the whole pitch is in view. In phone-only mode, look at "other movement ignored (last: ...)": it says why the ball was not counted. |
| Stumps not found automatically | Calibrate with nobody in front of the stumps and good light; light stumps against a light wall are hard to see. Or tap them yourself. |
| Windows says a DLL is missing | Unzip the whole zip, not just the exe. |
