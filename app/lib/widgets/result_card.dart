import 'package:flutter/material.dart';

import '../model/delivery.dart';

Color decisionColor(String? d) => switch (d) {
      'OUT' => Colors.red.shade700,
      'NOT OUT' => Colors.green.shade700,
      "UMPIRE'S CALL" => Colors.orange.shade800,
      _ => Colors.blueGrey,
    };

/// Compact result card for a delivery list.
class ResultCard extends StatelessWidget {
  const ResultCard({super.key, required this.delivery, this.onTap, this.onNoShotChanged});
  final Delivery delivery;
  final VoidCallback? onTap;
  final ValueChanged<bool>? onNoShotChanged;

  @override
  Widget build(BuildContext context) {
    final d = delivery;
    final lbw = d.decision;
    final t = Theme.of(context).textTheme;
    return Card(
      clipBehavior: Clip.antiAlias,
      child: InkWell(
        onTap: onTap,
        child: Padding(
          padding: const EdgeInsets.all(12),
          child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
            Row(children: [
              Text('Ball ${d.index}', style: t.titleMedium),
              const Spacer(),
              if (d.speed != null) Text('${d.speed!.value.toStringAsFixed(0)} km/h', style: t.titleMedium),
            ]),
            const SizedBox(height: 6),
            if (lbw.decision != null)
              Container(
                padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 4),
                decoration: BoxDecoration(color: decisionColor(lbw.decision), borderRadius: BorderRadius.circular(6)),
                child: Text('LBW: ${lbw.decision}', style: t.titleMedium!.copyWith(color: Colors.white)),
              )
            else
              Text(d.impactKind == 'stumps' ? 'Hit the stumps area (bowled?)' : 'No pad impact seen',
                  style: t.titleSmall),
            const SizedBox(height: 6),
            Wrap(spacing: 16, runSpacing: 4, children: [
              if (d.lbwApplicable) ...[
                _kv('Pitching', lbw.pitching),
                _kv('Impact', lbw.impact),
              ],
              _kv('Wickets', '${lbw.wickets} (${(lbw.pHit * 100).toStringAsFixed(0)}%)'),
              _kv('Length', d.lengthZone),
              if (d.lineZone.isNotEmpty) _kv('Line', d.lineZone),
              if (d.turn != null) _kv('Turn', '${d.turn!.value.toStringAsFixed(1)}° ${d.turnType}'),
              if (d.swing != null) _kv('Swing', '${d.swing!.value.toStringAsFixed(0)} cm ${d.swingType}'),
            ]),
            if (d.lbwApplicable && onNoShotChanged != null)
              Row(children: [
                const Text('No shot offered'),
                Switch(value: d.noShot, onChanged: onNoShotChanged),
              ]),
            for (final w in d.warnings)
              Padding(
                padding: const EdgeInsets.only(top: 4),
                child: Text('! $w', style: t.bodySmall!.copyWith(color: Colors.orange.shade900)),
              ),
          ]),
        ),
      ),
    );
  }

  static Widget _kv(String k, String v) => Text.rich(TextSpan(children: [
        TextSpan(text: '$k: ', style: const TextStyle(fontWeight: FontWeight.bold)),
        TextSpan(text: v),
      ]));
}
