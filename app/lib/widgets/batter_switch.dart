import 'package:flutter/material.dart';

/// Who is batting, switchable while tracking. With nobody batting choose "No
/// batter": otherwise a ball that hits the stumps can come out as an LBW decision
/// (the engine takes a ball it loses or sees deflected near the stumps for a pad hit).
class BatterSwitch extends StatelessWidget {
  const BatterSwitch({super.key, required this.value, required this.onChanged});
  final String value;
  final ValueChanged<String> onChanged;

  @override
  Widget build(BuildContext context) {
    return Wrap(crossAxisAlignment: WrapCrossAlignment.center, spacing: 8, children: [
      const Text('Batter:'),
      SegmentedButton<String>(
        segments: const [
          ButtonSegment(value: 'right', label: Text('Right')),
          ButtonSegment(value: 'left', label: Text('Left')),
          ButtonSegment(value: 'none', label: Text('Nobody (practice)')),
        ],
        selected: {value},
        onSelectionChanged: (v) => onChanged(v.first),
      ),
    ]);
  }
}
