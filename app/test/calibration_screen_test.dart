// Calibration screen with the real engine (needs POCKETEYE_LIB, see
// engine_integration_test.dart). The laptop + old phone case: the stumps are
// not found automatically and the bowler's stumps are out of the picture, so
// the user taps the batter's stumps and enters the three tape measurements.
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:pocketeye/engine/native.dart';
import 'package:pocketeye/model/session.dart';
import 'package:pocketeye/screens/calibration_screen.dart';

void main() {
  final skip = Platform.environment['POCKETEYE_LIB'] == null ? 'set POCKETEYE_LIB' : null;

  testWidgets('stumps not found: tap them, enter the measurements, save', (tester) async {
    const w = 1280, h = 720;
    // A plain grey picture: nothing to find.
    final jpeg = PocketEyeNative.instance.yuv420ToJpeg(Uint8List(w * h)..fillRange(0, w * h, 120),
        Uint8List(w * h ~/ 4)..fillRange(0, w * h ~/ 4, 128), Uint8List(w * h ~/ 4)..fillRange(0, w * h ~/ 4, 128),
        w, h, w, w ~/ 2, 1)!;
    final session = Session()
      ..lengthMeasured = false
      ..stumpHeight = 0.66
      ..stumpWidth = 0.21;
    await tester.binding.setSurfaceSize(const Size(1400, 1000));
    await tester.pumpWidget(MaterialApp(home: CalibrationScreen(session: session, jpeg: jpeg, width: w, height: h)));

    // The automatic search runs in another isolate.
    for (var i = 0; i < 50 && find.textContaining('Could not find').evaluate().isEmpty; ++i) {
      await tester.runAsync(() => Future<void>.delayed(const Duration(milliseconds: 100)));
      await tester.pump();
    }
    expect(find.textContaining('Could not find'), findsOneWidget);

    // Tap the batter's stumps (picture coordinates -> screen).
    final picture = find.byWidgetPredicate((wd) => wd is SizedBox && wd.width == w.toDouble());
    Offset at(double u, double v) {
      final r = tester.getRect(picture);
      return r.topLeft + Offset(u * r.width / w, v * r.height / h);
    }

    for (final p in const [(632.0, 360.0), (632.0, 326.0), (646.0, 360.0), (646.0, 326.0)]) {
      await tester.tapAt(at(p.$1, p.$2));
      await tester.pump();
    }
    for (var i = 0; i < 4; ++i) {
      await tester.tap(find.text('Skip (not visible)'));
      await tester.pump();
    }
    expect(find.text('Use'), findsOneWidget); // asks for the measurements
    final save = find.widgetWithText(FilledButton, 'Save calibration');
    expect(tester.widget<FilledButton>(save).onPressed, isNull);

    await tester.enterText(find.widgetWithText(TextField, 'Pitch length, stumps to stumps (m)'), '16');
    await tester.enterText(find.widgetWithText(TextField, 'Camera lens height (cm)'), '120');
    await tester.enterText(find.widgetWithText(TextField, "Camera behind bowler's stumps (cm)"), '150');
    await tester.tap(find.text('Use'));
    await tester.pump();
    expect(find.textContaining('Calibration OK'), findsOneWidget);
    expect(tester.widget<FilledButton>(save).onPressed, isNotNull);
  }, skip: skip != null);

  final scene = Platform.environment['POCKETEYE_SCENE'];
  final pic = scene == null ? null : File('$scene/calib.jpg');
  testWidgets('stumps found automatically: the user only checks and saves', (tester) async {
    final cfg = jsonDecode(File('$scene/config.json').readAsStringSync()) as Map<String, dynamic>;
    final session = Session()
      ..lengthMeasured = true
      ..pitchLength = (cfg['pitch']['length_m'] as num).toDouble()
      ..stumpHeight = (cfg['pitch']['stump_height_m'] as num).toDouble()
      ..stumpWidth = (cfg['pitch']['stump_width_m'] as num).toDouble()
      ..hfovDeg = (cfg['camera_hint']['hfov_deg'] as num).toDouble()
      // Given with the scene when the bowler's stumps are out of the picture.
      ..cameraHeight = (cfg['camera_height_m'] as num?)?.toDouble()
      ..cameraDistance = (cfg['camera_distance_m'] as num?)?.toDouble();
    await tester.binding.setSurfaceSize(const Size(1400, 1000));
    await tester.pumpWidget(MaterialApp(
        home: CalibrationScreen(
            session: session,
            jpeg: pic!.readAsBytesSync(),
            width: (cfg['image']['width'] as num).toInt(),
            height: (cfg['image']['height'] as num).toInt())));
    for (var i = 0; i < 100 && find.textContaining('Calibration OK').evaluate().isEmpty; ++i) {
      await tester.runAsync(() => Future<void>.delayed(const Duration(milliseconds: 100)));
      await tester.pump();
    }
    expect(find.textContaining('Stumps found automatically'), findsOneWidget);
    expect(find.textContaining('Calibration OK'), findsOneWidget);
    final save = find.widgetWithText(FilledButton, 'Save calibration');
    expect(tester.widget<FilledButton>(save).onPressed, isNotNull);
  }, skip: skip != null || pic == null || !pic.existsSync());

  testWidgets("stumps only at the batter's end: four points and the measurements", (tester) async {
    const w = 1280, h = 720;
    final jpeg = PocketEyeNative.instance.yuv420ToJpeg(Uint8List(w * h)..fillRange(0, w * h, 120),
        Uint8List(w * h ~/ 4)..fillRange(0, w * h ~/ 4, 128), Uint8List(w * h ~/ 4)..fillRange(0, w * h ~/ 4, 128),
        w, h, w, w ~/ 2, 1)!;
    final session = Session()
      ..bowlerStumps = false
      ..lengthMeasured = true
      ..pitchLength = 16
      ..cameraHeight = 1.2
      ..cameraDistance = 1.5
      ..stumpHeight = 0.66
      ..stumpWidth = 0.21
      // Points left over from a setup with two sets of stumps must not count.
      ..taps = {'bowler_left_top': [600.0, 650.0]}
      ..imageWidth = w
      ..imageHeight = h;
    await tester.binding.setSurfaceSize(const Size(1400, 1000));
    await tester.pumpWidget(MaterialApp(home: CalibrationScreen(session: session, jpeg: jpeg, width: w, height: h)));
    for (var i = 0; i < 50 && find.textContaining('Could not find').evaluate().isEmpty; ++i) {
      await tester.runAsync(() => Future<void>.delayed(const Duration(milliseconds: 100)));
      await tester.pump();
    }
    final picture = find.byWidgetPredicate((wd) => wd is SizedBox && wd.width == w.toDouble());
    Offset at(double u, double v) {
      final r = tester.getRect(picture);
      return r.topLeft + Offset(u * r.width / w, v * r.height / h);
    }

    for (final p in const [(632.0, 360.0), (632.0, 326.0), (646.0, 360.0), (646.0, 326.0)]) {
      await tester.tapAt(at(p.$1, p.$2));
      await tester.pump();
    }
    expect(find.text('Skip (not visible)'), findsNothing); // no bowler's-end steps
    await tester.tap(find.text('Check'));
    await tester.pump();
    expect(find.textContaining('Calibration OK'), findsOneWidget);
    final save = find.widgetWithText(FilledButton, 'Save calibration');
    expect(tester.widget<FilledButton>(save).onPressed, isNotNull);
  }, skip: skip != null);
}
