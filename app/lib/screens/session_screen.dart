import 'package:flutter/material.dart';

import '../model/session.dart';

/// Pitch, stumps, ball and batter settings.
class SessionScreen extends StatefulWidget {
  const SessionScreen({super.key, required this.session});
  final Session session;

  @override
  State<SessionScreen> createState() => _SessionScreenState();
}

class _SessionScreenState extends State<SessionScreen> {
  final _form = GlobalKey<FormState>();
  late final _length =
      TextEditingController(text: widget.session.lengthMeasured ? widget.session.pitchLength.toString() : '');
  late final _stumpH = TextEditingController(text: (widget.session.stumpHeight * 100).toStringAsFixed(1));
  late final _stumpW = TextEditingController(text: (widget.session.stumpWidth * 100).toStringAsFixed(1));
  late final _camH = TextEditingController(text: _cm(widget.session.cameraHeight));
  late final _camD = TextEditingController(text: _cm(widget.session.cameraDistance));
  late final _fov = TextEditingController(text: widget.session.hfovDeg.toStringAsFixed(0));
  late String _ball = widget.session.ball;
  late String _batter = widget.session.batter;
  late bool _bowlerStumps = widget.session.bowlerStumps;

  static String _cm(double? m) => m == null ? '' : (m * 100).toStringAsFixed(0);

  String? _number(String? v, double min, double max) {
    final x = double.tryParse(v ?? '');
    if (x == null) return 'Enter a number';
    if (x < min || x > max) return 'Between $min and $max';
    return null;
  }

  String? _optional(String? v, double min, double max) => (v ?? '').trim().isEmpty ? null : _number(v, min, max);

  Future<void> _save() async {
    if (!_form.currentState!.validate()) return;
    final s = widget.session;
    final oldGeometry = [s.pitchLength, s.stumpHeight, s.stumpWidth, s.lengthMeasured ? 1.0 : 0.0];
    final oldStumps = s.bowlerStumps;
    s.lengthMeasured = _length.text.trim().isNotEmpty;
    if (s.lengthMeasured) s.pitchLength = double.parse(_length.text);
    s.stumpHeight = double.parse(_stumpH.text) / 100;
    s.stumpWidth = double.parse(_stumpW.text) / 100;
    s.cameraHeight = _camH.text.trim().isEmpty ? null : double.parse(_camH.text) / 100;
    s.cameraDistance = _camD.text.trim().isEmpty ? null : double.parse(_camD.text) / 100;
    final fov = double.parse(_fov.text);
    if (fov.toStringAsFixed(0) != s.hfovDeg.toStringAsFixed(0)) {
      s.hfovDeg = fov;
      s.hfovSigmaDeg = null; // typed in: only a rough value
    }
    s.ball = _ball;
    s.batter = _batter;
    s.bowlerStumps = _bowlerStumps;
    await s.save();
    if (!mounted) return;
    final changed = oldGeometry[0] != s.pitchLength ||
        oldGeometry[1] != s.stumpHeight ||
        oldGeometry[2] != s.stumpWidth ||
        oldGeometry[3] != (s.lengthMeasured ? 1.0 : 0.0) ||
        oldStumps != s.bowlerStumps;
    Navigator.pop(context, changed);
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('Pitch and match setup')),
      body: Form(
        key: _form,
        child: ListView(
          padding: const EdgeInsets.all(16),
          children: [
            Text('Measure with a tape. Accurate numbers matter most for LBW.',
                style: Theme.of(context).textTheme.bodyMedium),
            const SizedBox(height: 12),
            SwitchListTile(
              contentPadding: EdgeInsets.zero,
              title: const Text("Stumps at the bowler's end too"),
              subtitle: Text(_bowlerStumps
                  ? 'Two sets of stumps.'
                  : "Only the batter's stumps: the bowler bowls from a mark. Measure the pitch length from that "
                      'mark, and the camera height and distance below.'),
              value: _bowlerStumps,
              onChanged: (v) => setState(() => _bowlerStumps = v),
            ),
            TextFormField(
              controller: _length,
              decoration: InputDecoration(
                  labelText: _bowlerStumps
                      ? 'Pitch length, stumps to stumps (m)'
                      : "Pitch length, bowling mark to the batter's stumps (m)",
                  helperText: _bowlerStumps
                      ? 'Full pitch: 20.12 m. Leave empty to measure it from the camera picture\n'
                          "(the bowler's stumps must be in the picture; a tape measure is more exact)."
                      : 'Needed: with one set of stumps the picture cannot give the length.',
                  helperMaxLines: 3),
              keyboardType: const TextInputType.numberWithOptions(decimal: true),
              validator: (v) => _bowlerStumps ? _optional(v, 5, 25) : _number(v, 5, 25),
            ),
            TextFormField(
              controller: _stumpH,
              decoration: const InputDecoration(
                  labelText: 'Stump height, ground to top of the stumps (cm)', helperText: 'Standard: 71.1'),
              keyboardType: const TextInputType.numberWithOptions(decimal: true),
              validator: (v) => _number(v, 30, 100),
            ),
            TextFormField(
              controller: _stumpW,
              decoration: const InputDecoration(
                  labelText: 'Stumps width, outer edge to outer edge (cm)', helperText: 'Standard: 22.9'),
              keyboardType: const TextInputType.numberWithOptions(decimal: true),
              validator: (v) => _number(v, 10, 40),
            ),
            const SizedBox(height: 16),
            DropdownButtonFormField<String>(
              initialValue: _ball,
              decoration: const InputDecoration(labelText: 'Ball'),
              items: [for (final b in ballTypes) DropdownMenuItem(value: b, child: Text(b))],
              onChanged: (v) => setState(() => _ball = v!),
            ),
            const SizedBox(height: 8),
            SegmentedButton<String>(
              segments: const [
                ButtonSegment(value: 'right', label: Text('Right-handed')),
                ButtonSegment(value: 'left', label: Text('Left-handed')),
                ButtonSegment(value: 'none', label: Text('No batter')),
              ],
              selected: {_batter},
              onSelectionChanged: (v) => setState(() => _batter = v.first),
            ),
            if (_batter == 'none')
              const Padding(
                padding: EdgeInsets.only(top: 4),
                child: Text('Practice at empty stumps: no LBW, the app says whether each ball would hit the stumps.'),
              ),
            const SizedBox(height: 24),
            Text(
                _bowlerStumps
                    ? "Only if the bowler's stumps are NOT visible in the camera picture:"
                    : 'Camera position (needed):',
                style: Theme.of(context).textTheme.titleSmall),
            TextFormField(
              controller: _camH,
              decoration: const InputDecoration(labelText: 'Camera lens height above ground (cm)'),
              keyboardType: const TextInputType.numberWithOptions(decimal: true),
              validator: (v) => _bowlerStumps ? _optional(v, 20, 400) : _number(v, 20, 400),
            ),
            TextFormField(
              controller: _camD,
              decoration: InputDecoration(
                  labelText: _bowlerStumps
                      ? "Camera distance behind the bowler's stumps (cm)"
                      : 'Camera distance behind the bowling mark (cm)'),
              keyboardType: const TextInputType.numberWithOptions(decimal: true),
              validator: (v) => _bowlerStumps ? _optional(v, 0, 1000) : _number(v, 0, 1000),
            ),
            const SizedBox(height: 16),
            TextFormField(
              controller: _fov,
              decoration: const InputDecoration(
                  labelText: 'Camera field of view (degrees)', helperText: 'Most phones: 60-70. Leave 65 if unsure.'),
              keyboardType: TextInputType.number,
              validator: (v) => _number(v, 30, 120),
            ),
            const SizedBox(height: 24),
            FilledButton(onPressed: _save, child: const Text('Save')),
          ],
        ),
      ),
    );
  }
}
