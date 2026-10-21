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

Easiest, including straight from the phone: open the repository on GitHub → **Releases** → **PocketEye latest build** and tap the file you need.

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

In the app, open **Pitch and match setup** and type:

- the pitch length (stumps to stumps, in metres) and the stump height and width, measured with a tape. Accurate numbers matter most for LBW;
- the ball type (tennis/tape, plastic, rubber, leather) and the batter (right- or left-handed).

If the bowler's stumps are not in the picture, also give the camera height and its distance behind the bowler's stumps.

## 4. Calibrate: tap the stumps

Every mode opens the same calibration screen on a still picture from the camera:

1. Zoom in (pinch or mouse wheel) and tap the requested point: bottom and top of the **left** and **right** outer stump at the batter's end ("left/right" as seen in the picture).
2. Then the bowler's-end stumps if they are visible, or **Skip (not visible)**.
3. Press **Check**. It shows the tap error and draws the stump lines and the pitch corridor in yellow. They should sit on the real stumps and pitch. **Undo** fixes a wrong tap.
4. **Save calibration**.

## 5a. Phone + laptop (old phones)

1. Phone and laptop on the **same Wi-Fi** (a phone hotspot that the laptop joins works too).
2. Laptop: **Live: phone camera to this laptop**. It shows a QR code and a 6-digit code.
3. Phone: open **PocketEye Camera** → **Scan laptop QR** and point it at the screen. If scanning fails, use **Type address** and enter the address, port and code shown under the QR code.
4. Put the phone on its stand. On the laptop press **Calibrate: tap the stumps**.
5. Bowl. Each delivery appears on the laptop as a card, and the same result card shows on the phone a moment later.
6. Click a card for the replay (slow motion, **Zoom on batter**), the pitch map and all measurements.

**Quality** on the phone sets the picture size: 1280×720 is the most accurate, and 960×540 or 640×480 helps on weak Wi-Fi. After changing it, calibrate again.

## 5b. Phone-only (newer phones)

On first launch the app runs a short speed test and suggests a mode:

- "phone-only 60 fps" or "phone-only 30 fps": this phone is fast enough;
- "laptop": this phone is too slow, so use the phone + laptop mode.

**Re-test** repeats it. Then:

1. **Phone-only mode** → put the phone on the stand → **Calibrate: tap the stumps**.
2. Bowl. Cards appear on the phone, and tapping one opens the replay.

The screen shows the engine time per frame and how many frames were skipped. If many frames are skipped, accuracy drops. Use the laptop mode for important games.

## 6. Recorded videos (laptop)

**Analyse a recorded video** → **Choose video** → **Calibrate (tap stumps)** → **Analyse**.
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
| No deliveries found | Check the calibration lines, the light and that the whole pitch is in view. |
| Windows says a DLL is missing | Unzip the whole zip, not just the exe. |
