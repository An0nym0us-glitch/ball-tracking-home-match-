import 'dart:async';
import 'dart:io';

import 'package:file_picker/file_picker.dart';
import 'package:flutter/material.dart';
import 'package:path_provider/path_provider.dart';

import '../engine/native.dart';
import '../engine/workers.dart';
import '../model/delivery.dart';
import '../model/session.dart';
import '../widgets/result_card.dart';
import 'calibration_screen.dart';
import 'delivery_screen.dart';

/// Laptop: analyse a recorded video (clip mode).
class FileScreen extends StatefulWidget {
  const FileScreen({super.key, required this.session});
  final Session session;

  @override
  State<FileScreen> createState() => _FileScreenState();
}

class _FileScreenState extends State<FileScreen> {
  String? _video;
  int _w = 0, _h = 0;
  double _duration = 0;
  VideoAnalysisJob? _job;
  Timer? _progressTimer;
  double _progress = 0;
  String? _error;
  String? _overlayPath;
  Map<String, dynamic>? _timings;
  final _deliveries = <Delivery>[];

  Session get s => widget.session;

  Future<void> _pick() async {
    final f = await FilePicker.pickFile(type: FileType.video, dialogTitle: 'Choose a cricket video');
    final path = f?.path;
    if (path == null) return;
    final v = NativeVideo.open(path);
    if (v == null) {
      setState(() => _error = 'Cannot open this video.');
      return;
    }
    setState(() {
      _video = path;
      _w = v.width;
      _h = v.height;
      _duration = v.duration;
      _error = null;
      _deliveries.clear();
      _overlayPath = null;
    });
    v.close();
  }

  Future<void> _calibrate() async {
    final v = NativeVideo.open(_video!);
    if (v == null) return;
    v.seek(_duration > 2 ? 1.0 : 0.0);
    final frame = v.readJpeg(quality: 92);
    v.close();
    if (frame == null || !mounted) return;
    await Navigator.push(context,
        MaterialPageRoute(builder: (_) => CalibrationScreen(session: s, jpeg: frame.$1, width: _w, height: _h)));
    setState(() {});
  }

  Future<void> _analyse() async {
    final dir = await getTemporaryDirectory();
    final stamp = DateTime.now().millisecondsSinceEpoch;
    final outJson = '${dir.path}${Platform.pathSeparator}pocketeye_$stamp.json';
    final overlay = '${dir.path}${Platform.pathSeparator}pocketeye_replay_$stamp.mp4';
    final job = VideoAnalysisJob(s.toConfigJson(), _video!, outJson, overlay);
    setState(() {
      _job = job;
      _progress = 0;
      _error = null;
      _deliveries.clear();
    });
    _progressTimer = Timer.periodic(const Duration(milliseconds: 200), (_) {
      if (mounted) setState(() => _progress = job.progress);
    });
    try {
      final result = await job.run();
      final list = (result['deliveries'] as List).cast<Map<String, dynamic>>();
      var i = 0;
      final ds = [for (final j in list) if (j['valid'] == true) Delivery(j, index: ++i)];
      _loadFrames(ds);
      setState(() {
        _deliveries.addAll(ds);
        _overlayPath = File(overlay).existsSync() ? overlay : null;
        _timings = (result['timings_ms'] as Map).cast<String, dynamic>();
      });
    } catch (e) {
      setState(() => _error = e.toString());
    } finally {
      _progressTimer?.cancel();
      setState(() => _job = null);
    }
  }

  /// Reads the frames around each delivery for the replay.
  void _loadFrames(List<Delivery> ds) {
    final v = NativeVideo.open(_video!);
    if (v == null) return;
    for (final d in ds) {
      v.seek((d.tFirst - 0.4).clamp(0, double.infinity).toDouble());
      final frames = <ReplayFrame>[];
      while (true) {
        final f = v.readJpeg(maxWidth: 960, quality: 80);
        if (f == null || f.$2 > d.tLast + 0.6) break;
        frames.add(ReplayFrame(f.$1, f.$2));
      }
      d.frames = frames;
    }
    v.close();
  }

  void _openReplayVideo() {
    final p = _overlayPath;
    if (p == null) return;
    if (Platform.isWindows) {
      Process.run('cmd', ['/c', 'start', '', p]);
    } else if (Platform.isLinux) {
      Process.run('xdg-open', [p]);
    }
  }

  @override
  void dispose() {
    _progressTimer?.cancel();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final calibrated = _video != null && s.isCalibrated && s.matchesSize(_w, _h);
    return Scaffold(
      appBar: AppBar(title: const Text('Analyse a video')),
      body: ListView(padding: const EdgeInsets.all(12), children: [
        Wrap(spacing: 8, runSpacing: 8, crossAxisAlignment: WrapCrossAlignment.center, children: [
          FilledButton.icon(onPressed: _job == null ? _pick : null, icon: const Icon(Icons.video_file), label: const Text('Choose video')),
          if (_video != null)
            OutlinedButton.icon(
                onPressed: _job == null ? _calibrate : null,
                icon: const Icon(Icons.straighten),
                label: Text(calibrated ? 'Re-calibrate' : 'Calibrate')),
          if (_video != null)
            FilledButton.icon(
                onPressed: calibrated && _job == null ? _analyse : null,
                icon: const Icon(Icons.play_arrow),
                label: const Text('Analyse')),
          if (_overlayPath != null)
            OutlinedButton.icon(onPressed: _openReplayVideo, icon: const Icon(Icons.movie), label: const Text('Open replay video')),
        ]),
        if (_video != null)
          Padding(
            padding: const EdgeInsets.only(top: 8),
            child: Text('$_video\n${_w}x$_h, ${_duration.toStringAsFixed(1)} s'
                '${calibrated ? '' : '\nCalibrate first: tap the stumps on a frame of this video.'}'),
          ),
        if (_job != null) ...[
          const SizedBox(height: 12),
          LinearProgressIndicator(value: _progress),
          Text('Analysing... ${(100 * _progress).toStringAsFixed(0)}%'),
        ],
        if (_error != null) Padding(padding: const EdgeInsets.all(8), child: Text(_error!, style: const TextStyle(color: Colors.red))),
        if (_timings != null)
          Text('Detection ${(_timings!['detect_per_frame'] as num).toStringAsFixed(1)} ms/frame, '
              'total ${((_timings!['wall_total'] as num) / 1000).toStringAsFixed(1)} s',
              style: Theme.of(context).textTheme.bodySmall),
        if (_job == null && _video != null && _deliveries.isEmpty && _timings != null)
          const Padding(padding: EdgeInsets.all(8), child: Text('No deliveries found in this video.')),
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
