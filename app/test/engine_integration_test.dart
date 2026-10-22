// Integration tests against the real engine library. Needs:
//   POCKETEYE_LIB=<path to libpocketeye_c.so>   (engine/build)
//   POCKETEYE_SCENE=<synthetic scene folder>      (tools/synth/generate.py)
// Skipped when these are not set.
import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:pocketeye/engine/native.dart';
import 'package:pocketeye/engine/workers.dart';
import 'package:pocketeye/model/delivery.dart';
import 'package:pocketeye/net/protocol.dart';

void main() {
  final lib = Platform.environment['POCKETEYE_LIB'];
  final scene = Platform.environment['POCKETEYE_SCENE'];
  final skip = lib == null || scene == null ? 'set POCKETEYE_LIB and POCKETEYE_SCENE' : null;

  String config() => File('$scene/config.json').readAsStringSync();

  test('calibration from stump taps', () {
    final r = PocketEyeNative.instance.calibrate(config());
    expect(r['ok'], isTrue, reason: r['error']?.toString());
    expect((r['rms_px'] as num).toDouble(), lessThan(3.0));
    expect(r['overlay'], isNotEmpty);
  }, skip: skip);

  test('bad config gives a readable error', () {
    final r = PocketEyeNative.instance.calibrate('{"image":{"width":1280,"height":720},"taps":{}}');
    expect(r['ok'], isFalse);
    expect(r['error'].toString(), contains('batter_left_base'));
  }, skip: skip);

  test('whole-video analysis finds the delivery; Dart LBW logic matches the engine', () async {
    final out = '${Directory.systemTemp.path}/pe_test_${DateTime.now().microsecondsSinceEpoch}.json';
    final job = VideoAnalysisJob(config(), '$scene/video.mp4', out, '');
    final res = await job.run();
    final valid = (res['deliveries'] as List).where((d) => d['valid'] == true).toList();
    expect(valid, isNotEmpty);
    for (final j in valid) {
      final d = Delivery(j as Map<String, dynamic>, index: 1);
      expect(d.decision.decision, j['lbw']['decision']);
      expect(d.path, isNotEmpty);
    }
  }, skip: skip, timeout: const Timeout(Duration(minutes: 3)));

  test('automatic stump calibration on the empty-pitch picture', () {
    final pic = File('$scene/calib.jpg');
    if (!pic.existsSync()) {
      markTestSkipped('scene has no calib.jpg (regenerate it with tools/synth/generate.py)');
      return;
    }
    final cfg = jsonDecode(config()) as Map<String, dynamic>..remove('taps');
    final r = PocketEyeNative.instance.autoCalibrate(jsonEncode(cfg), pic.readAsBytesSync());
    expect(r['batter_found'], isTrue, reason: r['message']?.toString());
    expect(r['ok'], isTrue, reason: r['message']?.toString());
    final taps = (r['taps'] as Map).keys.toSet();
    expect(taps, containsAll(['batter_left_base', 'batter_left_top', 'batter_right_base', 'batter_right_top']));
    // The engine accepts the found points as a calibration.
    final check = PocketEyeNative.instance.calibrate(jsonEncode({...cfg, 'taps': r['taps']}));
    expect(check['ok'], isTrue, reason: check['error']?.toString());
  }, skip: skip);

  test('video frame reader', () {
    final v = NativeVideo.open('$scene/video.mp4')!;
    expect(v.width, greaterThan(0));
    final f = v.readJpeg(maxWidth: 320);
    expect(f, isNotNull);
    expect(f!.$1.sublist(0, 2), [0xFF, 0xD8]); // JPEG
    v.close();
  }, skip: skip);

  test('laptop live mode: phone stream in, delivery and result card out', () async {
    final server = await LiveServer.start(token: '424242', port: 47911);
    final deliveries = Completer<DeliveryEvent>();
    server.events.listen((e) {
      if (e is DeliveryEvent && e.json['valid'] == true && !deliveries.isCompleted) deliveries.complete(e);
    });
    server.configure(config());

    // Fake phone: read the synthetic video, send JPEG frames with capture times.
    final ts = File('$scene/timestamps.txt').readAsLinesSync().where((l) => l.trim().isNotEmpty).map(double.parse).toList();
    final v = NativeVideo.open('$scene/video.mp4')!;
    final sock = await Socket.connect('127.0.0.1', 47911);
    final fromLaptop = <String>[];
    sock.cast<List<int>>().transform(utf8.decoder).transform(const LineSplitter()).listen(fromLaptop.add);
    sock.write('${jsonEncode({'hello': 'pocketeye-cam', 'token': '424242', 'width': v.width, 'height': v.height, 'fps': 30})}\n');
    Uint8List? last;
    var i = 0;
    for (;; ++i) {
      final f = v.readJpeg(quality: 92);
      if (f == null) break;
      last = f.$1;
      sock.add(encodeFrame((ts[i] * 1e6).round(), f.$1));
      if (i % 10 == 0) await sock.flush();
    }
    // Keep the stream going for a second (a real phone never stops).
    for (var k = 1; k <= 30; ++k) {
      sock.add(encodeFrame(((ts.last + k / 30.0) * 1e6).round(), last!));
    }
    await sock.flush();
    v.close();

    final d = await deliveries.future.timeout(const Duration(seconds: 60));
    expect(d.frames, isNotEmpty);
    await Future<void>.delayed(const Duration(milliseconds: 500));
    expect(fromLaptop.first, contains('hello_ok'));
    expect(fromLaptop.any((l) => l.contains('"type":"result"')), isTrue);
    await sock.close();
    await server.stop();
  }, skip: skip, timeout: const Timeout(Duration(minutes: 3)));

  test('benchmark returns a suggestion', () {
    final b = PocketEyeNative.instance.benchmark(width: 640, height: 360, frames: 40);
    expect(['phone_only_60fps', 'phone_only_30fps', 'laptop'], contains(b['suggestion']));
  }, skip: skip);

  test('stream parser handles split messages', () {
    final jpeg = Uint8List.fromList(List.generate(1000, (i) => i % 256));
    final bytes = Uint8List.fromList([...utf8.encode('{"hello":"x"}\n'), ...encodeFrame(123456, jpeg)]);
    final p = StreamParser();
    for (var i = 0; i < bytes.length; i += 7) {
      p.add(Uint8List.sublistView(bytes, i, i + 7 > bytes.length ? bytes.length : i + 7));
    }
    expect(p.takeHello(), '{"hello":"x"}');
    final m = p.next()!;
    expect(m.$1, 'frame');
    expect(m.$2, 123456);
    expect(m.$3, jpeg);
    expect(p.next(), isNull);
  });
}
