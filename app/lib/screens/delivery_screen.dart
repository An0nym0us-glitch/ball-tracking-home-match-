import 'package:flutter/material.dart';

import '../model/delivery.dart';
import '../model/session.dart';
import '../widgets/pitch_map.dart';
import '../widgets/replay_player.dart';
import '../widgets/result_card.dart';

/// One delivery: replay, LBW, measurements with their ranges, pitch map.
class DeliveryScreen extends StatefulWidget {
  const DeliveryScreen({super.key, required this.delivery, required this.session});
  final Delivery delivery;
  final Session session;

  @override
  State<DeliveryScreen> createState() => _DeliveryScreenState();
}

class _DeliveryScreenState extends State<DeliveryScreen> {
  @override
  Widget build(BuildContext context) {
    final d = widget.delivery;
    final s = widget.session;
    String est(Est? e, int digits, String unit) => e == null ? '-' : e.fmt(digits, unit);
    final rows = <(String, String)>[
      ('Release speed', est(d.speed, 0, ' km/h')),
      ('Average speed', est(d.avgSpeed, 0, ' km/h')),
      ('Swing', '${est(d.swing, 0, ' cm')} ${d.swingType}'),
      ('Turn', d.turn == null ? 'no bounce seen' : '${est(d.turn, 1, '°')} ${d.turnType}'),
      ('Length (from batter\'s stumps)', '${est(d.bounceLength, 2, ' m')} ${d.lengthZone}'),
      ('Line at bounce (+ = off side)', est(d.bounceLine, 0, ' cm')),
      ('Line at stumps', est(d.lineAtStumps, 0, ' cm')),
      ('Height at stumps', est(d.heightAtStumps, 0, ' cm')),
      ('Line at pad impact', est(d.impactLine, 0, ' cm')),
      ('Height at pad impact', est(d.impactHeight, 0, ' cm')),
    ];
    return Scaffold(
      appBar: AppBar(title: Text('Ball ${d.index}')),
      body: ListView(padding: const EdgeInsets.all(12), children: [
        ReplayPlayer(delivery: d, imageWidth: s.imageWidth, imageHeight: s.imageHeight),
        ResultCard(delivery: d, onNoShotChanged: (v) => setState(() => d.noShot = v)),
        Card(
          child: Padding(
            padding: const EdgeInsets.all(12),
            child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
              Text('Measurements (90 % range in brackets)', style: Theme.of(context).textTheme.titleSmall),
              const SizedBox(height: 6),
              for (final r in rows)
                Padding(
                  padding: const EdgeInsets.symmetric(vertical: 2),
                  child: Row(crossAxisAlignment: CrossAxisAlignment.start, children: [
                    SizedBox(width: 210, child: Text(r.$1)),
                    Expanded(child: Text(r.$2)),
                  ]),
                ),
            ]),
          ),
        ),
        Card(
          child: Padding(
            padding: const EdgeInsets.all(12),
            child: PitchMap(
              delivery: d,
              pitchLength: s.pitchLength,
              stumpWidth: s.stumpWidth,
              stumpHeight: s.stumpHeight,
              batter: s.batter,
            ),
          ),
        ),
      ]),
    );
  }
}
