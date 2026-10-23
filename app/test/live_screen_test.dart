// The laptop live screen with the real engine (needs POCKETEYE_LIB and
// POCKETEYE_SCENE, see engine_integration_test.dart): a fake old phone streams
// the scene's video; the screen must show the picture, the box on the ball
// and the delivery card.
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:pocketeye/engine/native.dart';
import 'package:pocketeye/model/session.dart';
import 'package:pocketeye/net/protocol.dart';
import 'package:pocketeye/screens/live_screen.dart';
import 'package:pocketeye/widgets/engine_overlay.dart';
import 'package:pocketeye/widgets/result_card.dart';

void main() {
  final lib = Platform.environment['POCKETEYE_LIB'];
  final scene = Platform.environment['POCKETEYE_SCENE'];

  testWidgets('live picture, ball box and delivery card', (tester) async {
    final cfg = jsonDecode(File('$scene/config.json').readAsStringSync()) as Map<String, dynamic>;
    final session = Session()
      ..imageWidth = (cfg['image']['width'] as num).toInt()
      ..imageHeight = (cfg['image']['height'] as num).toInt()
      ..lengthMeasured = true
      ..pitchLength = (cfg['pitch']['length_m'] as num).toDouble()
      ..stumpHeight = (cfg['pitch']['stump_height_m'] as num).toDouble()
      ..stumpWidth = (cfg['pitch']['stump_width_m'] as num).toDouble()
      ..ball = cfg['ball'] as String
      ..batter = cfg['batter'] as String
      // Given with the scene when the bowler's stumps are out of the picture.
      ..cameraHeight = (cfg['camera_height_m'] as num?)?.toDouble()
      ..cameraDistance = (cfg['camera_distance_m'] as num?)?.toDouble()
      ..taps = {
        for (final e in (cfg['taps'] as Map<String, dynamic>).entries)
          e.key: [for (final v in e.value as List) (v as num).toDouble()]
      };
    await tester.binding.setSurfaceSize(const Size(1600, 1200));
    await tester.pumpWidget(MaterialApp(home: LiveScreen(session: session)));
    Future<void> settle([int ms = 100]) async {
      await tester.runAsync(() => Future<void>.delayed(Duration(milliseconds: ms)));
      await tester.pump();
    }

    String? code;
    for (var i = 0; i < 50 && code == null; ++i) {
      await settle();
      final t = find.textContaining('code ');
      if (t.evaluate().isNotEmpty) code = RegExp(r'code (\d{6})').firstMatch((tester.widget<Text>(t.first)).data!)?.group(1);
    }
    expect(code, isNotNull);

    // Fake phone: hello, then the video as JPEG frames in real time; the
    // screen is redrawn between bursts of frames, as the app does.
    final ts = File('$scene/timestamps.txt').readAsLinesSync().where((l) => l.trim().isNotEmpty).map(double.parse).toList();
    final frames = <Uint8List>[];
    final v = NativeVideo.open('$scene/video.mp4')!;
    for (var f = v.readJpeg(quality: 85); f != null; f = v.readJpeg(quality: 85)) {
      frames.add(f.$1);
    }
    v.close();
    final sock = (await tester.runAsync(() => Socket.connect('127.0.0.1', defaultPort)))!;
    sock.listen((_) {});
    sock.write('${jsonEncode({'hello': 'pocketeye-cam', 'token': code, 'width': v.width, 'height': v.height, 'fps': 30})}\n');
    var ballShown = false, pictureShown = false;
    for (var i = 0; i < frames.length + 30; i += 3) {
      await tester.runAsync(() async {
        for (var k = i; k < i + 3; ++k) {
          final f = k < frames.length ? frames[k] : frames.last; // a real phone never stops
          final t = k < ts.length ? ts[k] : ts.last + (k - ts.length + 1) / 30.0;
          sock.add(encodeFrame((t * 1e6).round(), f));
        }
        await sock.flush();
        await Future<void>.delayed(const Duration(milliseconds: 100));
      });
      await tester.pump();
      pictureShown |= find.byType(Image).evaluate().isNotEmpty;
      for (final p in tester.widgetList<CustomPaint>(find.byType(CustomPaint))) {
        final painter = p.painter;
        if (painter is EngineOverlayPainter && painter.ball?['visible'] == true) ballShown = true;
      }
    }
    expect(pictureShown, isTrue, reason: 'live picture');
    for (var i = 0; i < 100 && find.byType(ResultCard).evaluate().isEmpty; ++i) {
      await settle();
    }
    expect(tester.takeException(), isNull);
    expect(ballShown, isTrue, reason: 'box on the ball in the live picture');
    expect(find.byType(ResultCard), findsOneWidget);
    expect(find.textContaining('Tracking is OFF'), findsNothing);
    if (Platform.environment['POCKETEYE_AI_MODEL'] != null) {
      // The AI assist runs (this machine has no GPU: the CPU, with a note).
      expect(find.textContaining('AI on '), findsOneWidget);
    }
    await tester.runAsync(() => sock.close());
    // Close the screen: it stops its server.
    await tester.pumpWidget(const SizedBox());
    await tester.runAsync(() => Future<void>.delayed(const Duration(milliseconds: 300)));
    await tester.pump(const Duration(milliseconds: 300));
  }, skip: lib == null || scene == null, timeout: const Timeout(Duration(minutes: 3)));
}
