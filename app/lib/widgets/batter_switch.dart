import 'package:flutter/material.dart';

/// Who is batting, switchable while tracking. With nobody batting choose "No
/// batter": otherwise a ball that hits the stumps can come out as an LBW decision
/// (the engine takes a ball it loses or sees deflected near the stumps for a pad hit).
/// "Auto" (with the AI assist): LBW only when a batter is seen at the stumps.
class BatterSwitch extends StatelessWidget {
  const BatterSwitch({super.key, required this.value, required this.onChanged, this.autoAvailable = false});
  final String value; // 'right', 'left', 'none' or 'auto'
  final ValueChanged<String> onChanged;
  final bool autoAvailable;

  @override
  Widget build(BuildContext context) {
    return Wrap(crossAxisAlignment: WrapCrossAlignment.center, spacing: 8, children: [
      const Text('Batter:'),
      SegmentedButton<String>(
        segments: [
          const ButtonSegment(value: 'right', label: Text('Right')),
          const ButtonSegment(value: 'left', label: Text('Left')),
          const ButtonSegment(value: 'none', label: Text('Nobody (practice)')),
          if (autoAvailable) const ButtonSegment(value: 'auto', label: Text('Auto (AI)')),
        ],
        selected: {value == 'auto' && !autoAvailable ? 'right' : value},
        onSelectionChanged: (v) => onChanged(v.first),
      ),
    ]);
  }
}
