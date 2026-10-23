import 'dart:convert';
import 'dart:io';

import 'package:path_provider/path_provider.dart';

import '../engine/ai.dart';

/// Stump tap points, in the order the calibration screen asks for them.
/// Left/right are as seen in the picture.
class TapSpec {
  const TapSpec(this.name, this.label, {this.required = false});
  final String name;
  final String label;
  final bool required;
}

const tapSpecs = <TapSpec>[
  TapSpec('batter_left_base', "Batter's end: LEFT stump, bottom", required: true),
  TapSpec('batter_left_top', "Batter's end: LEFT stump, top", required: true),
  TapSpec('batter_right_base', "Batter's end: RIGHT stump, bottom", required: true),
  TapSpec('batter_right_top', "Batter's end: RIGHT stump, top", required: true),
  TapSpec('bowler_left_top', "Bowler's end: LEFT stump, top (skip if not visible)"),
  TapSpec('bowler_right_top', "Bowler's end: RIGHT stump, top (skip if not visible)"),
  TapSpec('bowler_left_base', "Bowler's end: LEFT stump, bottom (skip if not visible)"),
  TapSpec('bowler_right_base', "Bowler's end: RIGHT stump, bottom (skip if not visible)"),
];

const ballTypes = ['tennis', 'plastic', 'rubber', 'leather'];

/// Everything the engine needs for one setup (pitch, stumps, camera taps, ball).
class Session {
  double pitchLength = 18.0; // stumps to stumps, metres
  /// True when the user measured the pitch length; otherwise it is measured
  /// from the picture (both sets of stumps visible) and pitchLength is that estimate.
  bool lengthMeasured = false;
  double stumpHeight = 0.71;
  double stumpWidth = 0.23; // outer edge to outer edge
  /// false: stumps only at the batter's end (the bowler bowls from a mark).
  bool bowlerStumps = true;
  String ball = 'tennis';
  String batter = 'right';
  /// Definite verdicts at 80 % certainty instead of 90 %: fewer umpire's calls,
  /// a few more wrong calls (docs/accuracy.md).
  bool boldDecisions = false;
  /// AI assist (laptop): YOLO finds people (bowler, batter) and checks spots
  /// near them for a ball; aiDevice: 'dgpu', 'igpu' or 'cpu'.
  bool aiEnabled = true;
  String aiDevice = 'dgpu';
  /// With the AI assist: LBW only when a batter is seen at the stumps, else
  /// practice (the batter's hand is [batter]).
  bool batterAuto = false;
  double? cameraHeight; // only needed if the bowler's stumps are out of frame
  double? cameraDistance;
  double hfovDeg = 65; // phone field of view (hint)
  /// How well hfovDeg is known (degrees): set from the phone's lens data; null = a rough guess.
  double? hfovSigmaDeg;
  /// Phone-only mode camera: '1080p' (sharper, needs a fast phone) or '720p'.
  String phoneVideo = '1080p';
  int imageWidth = 0;
  int imageHeight = 0;
  Map<String, List<double>> taps = {};

  bool get isCalibrated =>
      imageWidth > 0 && tapSpecs.where((t) => t.required).every((t) => taps.containsKey(t.name));

  bool matchesSize(int w, int h) => w == imageWidth && h == imageHeight;

  /// Engine session config (see engine/include/pocketeye/config.hpp). Without a
  /// measured length the engine measures it from the bowler's and batter's stumps.
  String toConfigJson() => jsonEncode({
        'image': {'width': imageWidth, 'height': imageHeight},
        'pitch': {
          if (lengthMeasured) 'length_m': pitchLength,
          'stump_height_m': stumpHeight,
          'stump_width_m': stumpWidth,
        },
        // Without stumps at the bowler's end, points tapped there earlier do not count.
        'taps': bowlerStumps ? taps : {for (final e in taps.entries) if (!e.key.startsWith('bowler')) e.key: e.value},
        if (!bowlerStumps) 'bowler_stumps': false,
        'camera_hint': {'hfov_deg': hfovDeg, if (hfovSigmaDeg != null) 'hfov_sigma_deg': hfovSigmaDeg},
        if (cameraHeight != null) 'camera_height_m': cameraHeight,
        if (cameraDistance != null) 'camera_distance_m': cameraDistance,
        'ball': ball,
        'batter': batter,
        if (boldDecisions) 'decision_probability': 0.8,
        if (aiActive) 'ai': {'model': aiModelPath(), 'device': aiDevice},
        if (aiActive && batterAuto && batter != 'none') 'batter_auto': true,
      });

  /// The AI assist is on and its model is installed (laptop app).
  bool get aiActive => aiEnabled && aiModelPath() != null;

  Map<String, dynamic> toJson() => {
        'pitchLength': pitchLength,
        'lengthMeasured': lengthMeasured,
        'bowlerStumps': bowlerStumps,
        'stumpHeight': stumpHeight,
        'stumpWidth': stumpWidth,
        'ball': ball,
        'batter': batter,
        'boldDecisions': boldDecisions,
        'aiEnabled': aiEnabled,
        'aiDevice': aiDevice,
        'batterAuto': batterAuto,
        'cameraHeight': cameraHeight,
        'cameraDistance': cameraDistance,
        'hfovDeg': hfovDeg,
        'hfovSigmaDeg': hfovSigmaDeg,
        'phoneVideo': phoneVideo,
        'imageWidth': imageWidth,
        'imageHeight': imageHeight,
        'taps': taps,
      };

  static Session fromJson(Map<String, dynamic> j) {
    final s = Session();
    double d(String k, double def) => (j[k] as num?)?.toDouble() ?? def;
    s.pitchLength = d('pitchLength', s.pitchLength);
    // Settings saved before the length could be measured from the picture were typed in.
    s.lengthMeasured = (j['lengthMeasured'] as bool?) ?? j.containsKey('pitchLength');
    s.bowlerStumps = (j['bowlerStumps'] as bool?) ?? true;
    s.stumpHeight = d('stumpHeight', s.stumpHeight);
    s.stumpWidth = d('stumpWidth', s.stumpWidth);
    s.ball = (j['ball'] as String?) ?? s.ball;
    s.batter = (j['batter'] as String?) ?? s.batter;
    s.boldDecisions = (j['boldDecisions'] as bool?) ?? false;
    s.aiEnabled = (j['aiEnabled'] as bool?) ?? true;
    s.aiDevice = (j['aiDevice'] as String?) ?? s.aiDevice;
    s.batterAuto = (j['batterAuto'] as bool?) ?? false;
    s.cameraHeight = (j['cameraHeight'] as num?)?.toDouble();
    s.cameraDistance = (j['cameraDistance'] as num?)?.toDouble();
    s.hfovDeg = d('hfovDeg', s.hfovDeg);
    s.hfovSigmaDeg = (j['hfovSigmaDeg'] as num?)?.toDouble();
    s.phoneVideo = (j['phoneVideo'] as String?) ?? s.phoneVideo;
    s.imageWidth = (j['imageWidth'] as num?)?.toInt() ?? 0;
    s.imageHeight = (j['imageHeight'] as num?)?.toInt() ?? 0;
    final taps = j['taps'] as Map<String, dynamic>? ?? {};
    s.taps = taps.map((k, v) => MapEntry(k, (v as List).map((e) => (e as num).toDouble()).toList()));
    return s;
  }

  static Future<File> _file() async {
    final dir = await getApplicationSupportDirectory();
    return File('${dir.path}${Platform.pathSeparator}session.json');
  }

  static Future<Session> load() async {
    try {
      final f = await _file();
      if (await f.exists()) return fromJson(jsonDecode(await f.readAsString()) as Map<String, dynamic>);
    } catch (_) {
      // Corrupt or unreadable settings: start fresh.
    }
    return Session();
  }

  Future<void> save() async {
    final f = await _file();
    await f.parent.create(recursive: true);
    await f.writeAsString(jsonEncode(toJson()));
  }
}
