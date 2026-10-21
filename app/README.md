# PocketEye app (Flutter: Windows laptop + Android phone)

- **Windows**: live mode (old phone streams to the laptop through `camera_lite`), and video-file analysis.
- **Android 7+**: phone-only mode (the phone films and analyses by itself), with a speed test on first launch.

The tracking engine (`../engine`) is used through `dart:ffi` (`lib/engine/native.dart`).
Built by `.github/workflows/apps.yml`. For a local Linux test run:

```
cmake -S ../engine -B ../engine/build -G Ninja && cmake --build ../engine/build
POCKETEYE_LIB=$PWD/../engine/build/libpocketeye_c.so POCKETEYE_SCENE=<synthetic scene> flutter test
```
