import 'dart:convert';
import 'dart:io';
import 'dart:isolate';

import 'package:flutter/material.dart';
import 'package:path_provider/path_provider.dart';

import '../engine/native.dart';
import '../model/session.dart';
import 'file_screen.dart';
import 'live_screen.dart';
import 'phone_screen.dart';
import 'session_screen.dart';

class HomeScreen extends StatefulWidget {
  const HomeScreen({super.key});

  @override
  State<HomeScreen> createState() => _HomeScreenState();
}

class _HomeScreenState extends State<HomeScreen> {
  Session? _session;
  Map<String, dynamic>? _bench;
  bool _benchRunning = false;
  String? _engineError;

  bool get _desktop => Platform.isWindows || Platform.isLinux || Platform.isMacOS;

  @override
  void initState() {
    super.initState();
    _load();
  }

  Future<void> _load() async {
    final s = await Session.load();
    try {
      PocketEyeNative.instance.version();
    } catch (e) {
      _engineError = 'Tracking engine not found: $e';
    }
    setState(() => _session = s);
    if (Platform.isAndroid && _engineError == null) {
      _bench = await _loadBenchmark();
      if (_bench == null) await _runBenchmark();
      if (mounted) setState(() {});
    }
  }

  Future<File> _benchFile() async =>
      File('${(await getApplicationSupportDirectory()).path}${Platform.pathSeparator}benchmark.json');

  Future<Map<String, dynamic>?> _loadBenchmark() async {
    final f = await _benchFile();
    if (!await f.exists()) return null;
    return jsonDecode(await f.readAsString()) as Map<String, dynamic>;
  }

  /// First launch on a phone: time the engine and suggest a mode.
  Future<void> _runBenchmark() async {
    setState(() => _benchRunning = true);
    final r = await Isolate.run(() => PocketEyeNative.instance.benchmark(width: 1280, height: 720, frames: 150));
    final f = await _benchFile();
    await f.parent.create(recursive: true);
    await f.writeAsString(jsonEncode(r));
    if (mounted) {
      setState(() {
        _bench = r;
        _benchRunning = false;
      });
    }
  }

  void _open(Widget Function(Session s) page) async {
    await Navigator.push(context, MaterialPageRoute(builder: (_) => page(_session!)));
    setState(() {});
  }

  @override
  Widget build(BuildContext context) {
    final s = _session;
    final t = Theme.of(context).textTheme;
    return Scaffold(
      appBar: AppBar(title: const Text('PocketEye: LBW ball tracking')),
      body: s == null
          ? const Center(child: CircularProgressIndicator())
          : ListView(padding: const EdgeInsets.all(16), children: [
              if (_engineError != null)
                Card(color: Colors.red.shade50, child: Padding(padding: const EdgeInsets.all(12), child: Text(_engineError!))),
              Card(
                child: ListTile(
                  leading: const Icon(Icons.settings),
                  title: const Text('Pitch and match setup'),
                  subtitle: Text('Pitch ${s.pitchLength.toStringAsFixed(2)} m, stumps '
                      '${(s.stumpHeight * 100).toStringAsFixed(0)}x${(s.stumpWidth * 100).toStringAsFixed(0)} cm, '
                      '${s.ball} ball, ${s.batter}-handed batter\n'
                      '${s.isCalibrated ? 'Calibrated for ${s.imageWidth}x${s.imageHeight}' : 'Not calibrated yet'}'),
                  isThreeLine: true,
                  onTap: () => _open((s) => SessionScreen(session: s)),
                ),
              ),
              const SizedBox(height: 8),
              if (_desktop) ...[
                _modeCard(
                  icon: Icons.wifi,
                  title: 'Live: phone camera to this laptop',
                  text: 'For old phones: the phone only films (PocketEye Camera app) and this laptop tracks every ball. '
                      'Results appear here and on the phone.',
                  onTap: () => _open((s) => LiveScreen(session: s)),
                ),
                _modeCard(
                  icon: Icons.video_file,
                  title: 'Analyse a recorded video',
                  text: 'Choose a video filmed from behind the bowler\'s stumps, calibrate once, and get every delivery.',
                  onTap: () => _open((s) => FileScreen(session: s)),
                ),
              ],
              if (Platform.isAndroid) ...[
                if (_benchRunning)
                  const Card(child: ListTile(leading: CircularProgressIndicator(), title: Text('Testing this phone\'s speed...'))),
                if (_bench != null) _benchCard(t),
                _modeCard(
                  icon: Icons.phone_android,
                  title: 'Phone-only mode',
                  text: 'This phone films and analyses by itself (needs a fast phone, Android 7+).',
                  onTap: () => _open((s) => PhoneScreen(
                      session: s, fps: _bench?['suggestion'] == 'phone_only_60fps' ? 60 : 30)),
                ),
                const Card(
                  child: ListTile(
                    leading: Icon(Icons.laptop),
                    title: Text('Phone + laptop mode'),
                    subtitle: Text('For slow or old phones: install "PocketEye Camera" on the phone and open '
                        '"Live: phone camera to this laptop" in the Windows app.'),
                  ),
                ),
              ],
              const SizedBox(height: 16),
              Text('Camera setup', style: t.titleMedium),
              const Text('Put the phone on a stand behind the bowler\'s stumps, about 1-1.5 m high, looking down the '
                  'pitch. It must not move after calibration. Good light and 60 fps give the best results.'),
            ]),
    );
  }

  Widget _benchCard(TextTheme t) {
    final b = _bench!;
    final ms = (b['engine_ms_per_frame'] as num?)?.toDouble() ?? 0;
    final sug = b['suggestion'] as String? ?? 'laptop';
    final text = switch (sug) {
      'phone_only_60fps' => 'Fast phone: phone-only mode at 60 fps is recommended.',
      'phone_only_30fps' => 'Medium phone: phone-only mode works at 30 fps. Laptop mode is more accurate.',
      _ => 'This phone is too slow for phone-only mode. Use phone + laptop mode.',
    };
    return Card(
      color: sug == 'laptop' ? Colors.orange.shade50 : Colors.green.shade50,
      child: ListTile(
        leading: const Icon(Icons.speed),
        title: Text(text),
        subtitle: Text('Speed test: ${ms.toStringAsFixed(1)} ms per 720p frame'),
        trailing: TextButton(onPressed: _benchRunning ? null : _runBenchmark, child: const Text('Re-test')),
      ),
    );
  }

  Widget _modeCard({required IconData icon, required String title, required String text, required VoidCallback onTap}) {
    return Card(
      child: ListTile(
        leading: Icon(icon, size: 36),
        title: Text(title),
        subtitle: Text(text),
        onTap: _engineError == null ? onTap : null,
      ),
    );
  }
}
