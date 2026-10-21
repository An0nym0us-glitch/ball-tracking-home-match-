import 'dart:async';
import 'dart:typed_data';

import 'package:camera/camera.dart';
import 'package:flutter/material.dart';

import '../engine/native.dart';
import '../engine/workers.dart';
import '../model/delivery.dart';
import '../model/session.dart';
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
  double _engineMs = 0;
  bool _wantSnapshot = false;
  final _deliveries = <Delivery>[];
  bool _engineOn = false;

  Session get s => widget.session;

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
    _init();
  }

  Future<void> _init() async {
    try {
      final cams = await availableCameras();
      final back = cams.firstWhere((c) => c.lensDirection == CameraLensDirection.back, orElse: () => cams.first);
      CameraController cam;
      try {
        cam = CameraController(back, ResolutionPreset.high,
            enableAudio: false, fps: widget.fps, imageFormatGroup: ImageFormatGroup.yuv420);
        await cam.initialize();
      } catch (_) {
        // Not every phone offers 60 fps at 720p: fall back to the default rate.
        cam = CameraController(back, ResolutionPreset.high,
            enableAudio: false, imageFormatGroup: ImageFormatGroup.yuv420);
        await cam.initialize();
      }
      final worker = await PhoneEngineWorker.start();
      _sub = worker.events.listen(_onEvent);
      setState(() {
        _cam = cam;
        _worker = worker;
        _status = 'Camera ready';
      });
      await cam.startImageStream(_onImage);
    } catch (e) {
      setState(() => _status = 'Camera error: $e');
    }
  }

  void _onImage(CameraImage img) {
    // Arrival time on the phone: a few ms of jitter, see docs/accuracy.md.
    final t = _clock.elapsedMicroseconds / 1e6;
    _received++;
    if (img.planes.length < 3) return;
    final y = img.planes[0], u = img.planes[1], v = img.planes[2];
    if (_w != img.width || _h != img.height) {
      _w = img.width;
      _h = img.height;
      _maybeStartEngine();
    }
    if (_wantSnapshot) {
      _wantSnapshot = false;
      final jpeg = PocketEyeNative.instance.yuv420ToJpeg(y.bytes, u.bytes, v.bytes, img.width, img.height,
          y.bytesPerRow, u.bytesPerRow, u.bytesPerPixel ?? 1);
      if (jpeg != null) _openCalibration(jpeg, img.width, img.height);
      return;
    }
    if (!_engineOn) return;
    final sent = _worker!.offer(y.bytes, u.bytes, v.bytes, img.width, img.height, y.bytesPerRow, u.bytesPerRow,
        u.bytesPerPixel ?? 1, t);
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
        if (_received % 15 == 0) setState(() {});
      case DeliveryEvent():
        if (e.json['valid'] != true) return;
        final d = Delivery(e.json, index: _deliveries.length + 1)..frames = e.frames;
        setState(() => _deliveries.insert(0, d));
      case ErrorEvent():
        setState(() => _status = e.message);
      case PreviewEvent():
        break;
    }
  }

  void _maybeStartEngine() {
    if (s.isCalibrated && s.matchesSize(_w, _h)) {
      _worker?.configure(s.toConfigJson());
      _engineOn = true;
    } else {
      _engineOn = false;
      if (mounted) setState(() => _status = 'Calibrate first: tap the stumps');
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
        Text('${_w}x$_h  |  engine ${_engineMs.toStringAsFixed(1)} ms/frame  |  frames skipped ${dropPct.toStringAsFixed(0)}%',
            style: t.bodySmall),
        if (dropPct > 20)
          Text('This phone is too slow for every frame. Use the laptop mode for best accuracy.',
              style: t.bodySmall!.copyWith(color: Colors.orange.shade900)),
        const SizedBox(height: 8),
        FilledButton.icon(
          onPressed: cam == null ? null : () => _wantSnapshot = true,
          icon: const Icon(Icons.straighten),
          label: Text(_engineOn ? 'Re-calibrate' : 'Calibrate: tap the stumps'),
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
