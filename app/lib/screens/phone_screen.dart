import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:isolate';

import 'package:archive/archive_io.dart';
import 'package:camera/camera.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:path_provider/path_provider.dart';
import 'package:share_plus/share_plus.dart';

import '../engine/native.dart';
import '../engine/workers.dart';
import '../model/delivery.dart';
import '../model/session.dart';
import '../widgets/batter_switch.dart';
import '../widgets/engine_overlay.dart';
import '../widgets/result_card.dart';
import 'calibration_screen.dart';
import 'delivery_screen.dart';

/// Phone-only mode (strong phones): the phone films and analyses by itself.
class PhoneScreen extends StatefulWidget {
  const PhoneScreen({super.key, required this.session, this.fps = 60});
  final Session session;
  final int fps;

  @override
  State<PhoneScreen> createState() => _PhoneScreenState();
}

class _PhoneScreenState extends State<PhoneScreen> with WidgetsBindingObserver {
  CameraController? _cam;
  PhoneEngineWorker? _worker;
  StreamSubscription<WorkerEvent>? _sub;
  final _clock = Stopwatch()..start();
  String _status = 'Starting camera...';
  int _w = 0, _h = 0;
  int _received = 0, _dropped = 0;
  double _cameraFps = 0; // frames the camera actually delivers per second
  double _fpsT0 = 0;
  int _fpsCount = 0;
  double _engineMs = 0;
  bool _wantSnapshot = false;
  final _deliveries = <Delivery>[];
  bool _engineOn = false;
  Map<String, dynamic> _engineStats = const {};
  int _rotation = 0; // turns the sensor picture upright (clockwise degrees)
  bool _debugView = false;
  DebugEvent? _debug;
  bool _recording = false;

  Session get s => widget.session;

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
    // One fixed landscape orientation: the picture the engine gets (and the
    // calibration) must not turn when the phone is moved.
    SystemChrome.setPreferredOrientations([DeviceOrientation.landscapeLeft]);
    _init();
  }

  Future<void> _init() async {
    try {
      final worker = await PhoneEngineWorker.start();
      _sub = worker.events.listen(_onEvent);
      _worker = worker;
      await _startCamera();
    } catch (e) {
      setState(() => _status = 'Camera error: $e');
    }
  }

  /// Opens the back camera at the chosen size, at 60 fps if the phone allows it
  /// at that size, else the next best: 1080p60, 720p60, 1080p30, 720p30.
  Future<void> _startCamera() async {
    final cams = await availableCameras();
    final back = cams.firstWhere((c) => c.lensDirection == CameraLensDirection.back, orElse: () => cams.first);
    final sizes = s.phoneVideo == '720p'
        ? [ResolutionPreset.high]
        : [ResolutionPreset.veryHigh, ResolutionPreset.high];
    final attempts = <(ResolutionPreset, int?)>[
      for (final r in sizes) (r, widget.fps),
      for (final r in sizes) (r, null),
    ];
    CameraController? cam;
    Object? lastError;
    for (final (preset, fps) in attempts) {
      final c = CameraController(back, preset, enableAudio: false, fps: fps, imageFormatGroup: ImageFormatGroup.yuv420);
      try {
        await c.initialize();
        cam = c;
        break;
      } catch (e) {
        lastError = e;
        await c.dispose();
      }
    }
    if (cam == null) throw lastError ?? StateError('no camera');
    if (!mounted) {
      await cam.dispose();
      return;
    }
    // Back camera in landscape (Android ROTATION_90): the sensor picture needs
    // (sensor orientation - 90) degrees clockwise to be upright; 0 on most phones.
    _rotation = ((cam.description.sensorOrientation - 90) % 360 + 360) % 360;
    setState(() {
      _cam = cam;
      _status = 'Camera ready';
    });
    await cam.startImageStream(_onImage);
  }

  Future<void> _switchVideo(String v) async {
    if (v == s.phoneVideo) return;
    s.phoneVideo = v;
    await s.save();
    final old = _cam;
    setState(() {
      _cam = null;
      _engineOn = false;
      _w = _h = 0;
      _status = 'Switching camera...';
    });
    try {
      await old?.stopImageStream();
    } catch (_) {}
    await old?.dispose();
    try {
      await _startCamera();
    } catch (e) {
      if (mounted) setState(() => _status = 'Camera error: $e');
    }
  }

  void _onImage(CameraImage img) {
    // Arrival time on the phone: a few ms of jitter, see docs/accuracy.md.
    final t = _clock.elapsedMicroseconds / 1e6;
    _received++;
    if (++_fpsCount >= 30) {
      if (_fpsT0 > 0 && t > _fpsT0) _cameraFps = _fpsCount / (t - _fpsT0);
      _fpsT0 = t;
      _fpsCount = 0;
    }
    if (img.planes.length < 3) return;
    final y = img.planes[0], u = img.planes[1], v = img.planes[2];
    // Size of the upright picture.
    final turn = _rotation == 90 || _rotation == 270;
    final w = turn ? img.height : img.width, h = turn ? img.width : img.height;
    if (_w != w || _h != h) {
      _w = w;
      _h = h;
      _readLensFov();
      _maybeStartEngine();
    }
    if (_wantSnapshot) {
      _wantSnapshot = false;
      final jpeg = PocketEyeNative.instance.yuv420ToJpeg(y.bytes, u.bytes, v.bytes, img.width, img.height,
          y.bytesPerRow, u.bytesPerRow, u.bytesPerPixel ?? 1,
          rotation: _rotation);
      if (jpeg != null) _openCalibration(jpeg, w, h);
      return;
    }
    if (!_engineOn) return;
    final sent = _worker!.offer(y.bytes, u.bytes, v.bytes, img.width, img.height, y.bytesPerRow, u.bytesPerRow,
        u.bytesPerPixel ?? 1, t,
        rotation: _rotation);
    if (!sent) _dropped++;
  }

  void _onEvent(WorkerEvent e) {
    if (!mounted) return;
    switch (e) {
      case StatusEvent():
        setState(() => _status = e.text);
      case StatsEvent():
        final ms = e.stats['engine_ms'];
        if (ms is num) _engineMs = 0.9 * _engineMs + 0.1 * ms;
        if (e.stats.containsKey('rejected')) _engineStats = e.stats;
        if (_received % 15 == 0) setState(() {});
      case DeliveryEvent():
        if (e.json['valid'] != true) return;
        final d = Delivery(e.json, index: _deliveries.length + 1)..frames = e.frames;
        setState(() => _deliveries.insert(0, d));
      case ErrorEvent():
        setState(() => _status = e.message);
      case DebugEvent():
        if (_debugView) setState(() => _debug = e);
      case RecordedEvent():
        _shareClip(e.dir, e.frames);
      case PreviewEvent():
        break;
    }
  }

  void _toggleDebug(bool on) {
    _worker?.showDebug(on);
    setState(() {
      _debugView = on;
      if (!on) _debug = null;
    });
  }

  /// Records a few seconds of exactly what the engine gets, to send to the
  /// developers when balls are not found.
  Future<void> _recordClip() async {
    final base = await getApplicationDocumentsDirectory();
    final stamp = DateTime.now().toIso8601String().replaceAll(':', '-').split('.').first;
    final dir = Directory('${base.path}/clips/clip_$stamp');
    await dir.create(recursive: true);
    final cfg = jsonDecode(s.toConfigJson()) as Map<String, dynamic>;
    cfg['recorded'] = {
      'camera_fps': double.parse(_cameraFps.toStringAsFixed(1)),
      'frames_skipped_pct': _received == 0 ? 0 : (100 * _dropped / _received).round(),
      'engine_stats': _engineStats,
    };
    await File('${dir.path}/config.json').writeAsString(const JsonEncoder.withIndent('  ').convert(cfg));
    _worker?.record(dir.path, 8.0);
    setState(() {
      _recording = true;
      _status = 'Recording 8 s: bowl now...';
    });
  }

  Future<void> _shareClip(String dir, int frames) async {
    setState(() => _status = 'Packing the clip ($frames frames)...');
    try {
      final zip = await _zipFolder(dir);
      if (!mounted) return;
      setState(() {
        _recording = false;
        _status = 'Clip saved. Send it with the share menu.';
      });
      await SharePlus.instance.share(ShareParams(
          files: [XFile(zip)], text: 'PocketEye test clip ($frames frames, ${_w}x$_h)'));
    } catch (err) {
      if (mounted) {
        setState(() {
          _recording = false;
          _status = 'Could not save the clip: $err';
        });
      }
    }
  }

  void _maybeStartEngine() {
    if (s.isCalibrated && s.matchesSize(_w, _h)) {
      _worker?.configure(s.toConfigJson());
      _engineOn = true;
      _lockCamera();
    } else {
      _engineOn = false;
      if (mounted) setState(() => _status = 'Calibrate first');
    }
  }

  static const _lens = MethodChannel('pocketeye/lens');

  /// The exact field of view from the phone's lens data (Android): the pitch
  /// length can only be measured from the picture as well as this is known.
  Future<void> _readLensFov() async {
    try {
      final r = await _lens.invokeMapMethod<String, dynamic>('backCameraFov', {'width': _w, 'height': _h});
      final fov = (r?['hfov_deg'] as num?)?.toDouble();
      if (fov == null || fov < 30 || fov > 120) return;
      s.hfovDeg = fov;
      s.hfovSigmaDeg = (r?['sigma_deg'] as num?)?.toDouble() ?? 1.5;
      await s.save();
    } catch (_) {
      // Not available: the typed or default value stays.
    }
  }

  /// Automatic exposure jumps when the bowler runs past the camera, and every
  /// jump changes the whole picture and hides the ball. Meter and focus on the
  /// batter's stumps once, then keep the settings fixed (re-calibrate if the
  /// light changes a lot).
  Future<void> _lockCamera() async {
    final cam = _cam;
    final a = s.taps['batter_left_base'], b = s.taps['batter_right_top'];
    if (cam == null || a == null || b == null || _w == 0 || _h == 0) return;
    final p = Offset(((a[0] + b[0]) / 2 / _w).clamp(0.0, 1.0), ((a[1] + b[1]) / 2 / _h).clamp(0.0, 1.0));
    try {
      await cam.setExposureMode(ExposureMode.auto);
      await cam.setFocusMode(FocusMode.auto);
      await cam.setExposurePoint(p);
      await cam.setFocusPoint(p);
      await Future<void>.delayed(const Duration(milliseconds: 1500));
      await cam.setExposureMode(ExposureMode.locked);
      await cam.setFocusMode(FocusMode.locked);
    } catch (_) {
      // Not every phone supports metering points or locking: keep going.
    }
  }

  Future<void> _openCalibration(Uint8List jpeg, int w, int h) async {
    final saved = await Navigator.push<bool>(
        context, MaterialPageRoute(builder: (_) => CalibrationScreen(session: s, jpeg: jpeg, width: w, height: h)));
    if (saved == true) _maybeStartEngine();
    if (mounted) setState(() {});
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    if (state == AppLifecycleState.inactive || state == AppLifecycleState.paused) {
      _cam?.stopImageStream().catchError((_) {});
    } else if (state == AppLifecycleState.resumed && _cam != null && !_cam!.value.isStreamingImages) {
      _cam!.startImageStream(_onImage);
    }
  }

  @override
  void dispose() {
    SystemChrome.setPreferredOrientations([]);
    WidgetsBinding.instance.removeObserver(this);
    _sub?.cancel();
    _worker?.stop();
    _cam?.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final cam = _cam;
    final t = Theme.of(context).textTheme;
    final dropPct = _received == 0 ? 0 : 100 * _dropped / _received;
    return Scaffold(
      appBar: AppBar(title: const Text('Phone-only mode')),
      body: ListView(padding: const EdgeInsets.all(8), children: [
        if (cam != null && cam.value.isInitialized)
          AspectRatio(
            aspectRatio: cam.value.aspectRatio > 1 ? cam.value.aspectRatio : 1 / cam.value.aspectRatio,
            child: CameraPreview(cam),
          ),
        Padding(
          padding: const EdgeInsets.symmetric(vertical: 8),
          child: Text(_status, style: t.titleSmall),
        ),
        Row(children: [
          const Text('Camera: '),
          SegmentedButton<String>(
            segments: const [
              ButtonSegment(value: '1080p', label: Text('1080p')),
              ButtonSegment(value: '720p', label: Text('720p')),
            ],
            selected: {s.phoneVideo},
            onSelectionChanged: (v) => _switchVideo(v.first),
          ),
        ]),
        Text('${_w}x$_h at ${_cameraFps.toStringAsFixed(0)} fps  |  engine ${_engineMs.toStringAsFixed(1)} ms/frame  |  '
            'frames skipped ${dropPct.toStringAsFixed(0)}%',
            style: t.bodySmall),
        if (_engineStats.isNotEmpty)
          Text(
              'Balls found ${_engineStats['deliveries'] ?? 0}  |  other movement ignored ${_engineStats['rejected'] ?? 0}'
              '${(_engineStats['last_reject'] as String? ?? '').isNotEmpty ? ' (last: ${_engineStats['last_reject']})' : ''}'
              '${((_engineStats['analyses_waiting'] as num?) ?? 0) > 0 ? '  |  analysing...' : ''}',
              style: t.bodySmall),
        if (_engineOn)
          BatterSwitch(
            value: s.batter,
            onChanged: (v) {
              setState(() => s.batter = v);
              s.save();
              _worker?.configure(s.toConfigJson()); // new settings for the next ball
            },
          ),
        Row(children: [
          Expanded(
            child: SwitchListTile(
              dense: true,
              contentPadding: EdgeInsets.zero,
              title: const Text('Show what the engine sees'),
              value: _debugView,
              onChanged: _engineOn ? _toggleDebug : null,
            ),
          ),
          const SizedBox(width: 8),
          OutlinedButton.icon(
            onPressed: _engineOn && !_recording ? _recordClip : null,
            icon: const Icon(Icons.fiber_manual_record, color: Colors.red),
            label: const Text('Record test clip'),
          ),
        ]),
        if (_debugView && _debug != null) _DebugView(_debug!),
        if (dropPct > 20)
          Text('This phone is too slow for every frame. Use the laptop mode for best accuracy.',
              style: t.bodySmall!.copyWith(color: Colors.orange.shade900)),
        const SizedBox(height: 8),
        FilledButton.icon(
          onPressed: cam == null ? null : () => _wantSnapshot = true,
          icon: const Icon(Icons.straighten),
          label: Text(_engineOn ? 'Re-calibrate' : 'Calibrate'),
        ),
        const SizedBox(height: 8),
        for (final d in _deliveries)
          ResultCard(
            delivery: d,
            onNoShotChanged: (v) => setState(() => d.noShot = v),
            onTap: () => Navigator.push(
                context, MaterialPageRoute(builder: (_) => DeliveryScreen(delivery: d, session: s))),
          ),
      ]),
    );
  }
}

/// Packs a recorded clip folder into one zip (next to it). Top-level: runs in
/// a worker isolate.
Future<String> _zipFolder(String dir) => Isolate.run(() {
      final zipPath = '$dir.zip';
      final enc = ZipFileEncoder();
      enc.create(zipPath, level: 0); // JPEGs do not compress further
      for (final f in Directory(dir).listSync().whereType<File>().toList()..sort((a, b) => a.path.compareTo(b.path))) {
        enc.addFileSync(f, f.uri.pathSegments.last);
      }
      enc.closeSync();
      return zipPath;
    });

/// The last processed picture with the watched corridor (green) and the ball
/// candidates of the last second (yellow; the newest brightest).
class _DebugView extends StatelessWidget {
  const _DebugView(this.e);
  final DebugEvent e;

  @override
  Widget build(BuildContext context) {
    final w = (e.info['width'] as num?)?.toDouble() ?? 1, h = (e.info['height'] as num?)?.toDouble() ?? 1;
    final n = (e.info['candidates'] as List? ?? const []).length;
    return Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
      AspectRatio(
        aspectRatio: w / h,
        child: Stack(fit: StackFit.expand, children: [
          Image.memory(e.jpeg, fit: BoxFit.fill, gaplessPlayback: true),
          CustomPaint(painter: EngineOverlayPainter(width: w, height: h, debug: e.info)),
        ]),
      ),
      Text('Green: area watched for the ball. Yellow: possible balls in the last second ($n). '
          'A thrown ball should leave a line of yellow dots.'),
    ]);
  }
}
