import 'dart:io';

/// The YOLO model for the AI assist (laptop app): models/yolox_nano.onnx next to
/// the program, or POCKETEYE_AI_MODEL. Null when there is none (phone apps).
String? aiModelPath() {
  final env = Platform.environment['POCKETEYE_AI_MODEL'];
  if (env != null && File(env).existsSync()) return env;
  if (!(Platform.isWindows || Platform.isLinux || Platform.isMacOS)) return null;
  final sep = Platform.pathSeparator;
  final p = '${File(Platform.resolvedExecutable).parent.path}${sep}models${sep}yolox_nano.onnx';
  return File(p).existsSync() ? p : null;
}

/// Processor choices for the AI assist (engine "ai.device").
const aiDevices = {'dgpu': 'Dedicated GPU', 'igpu': 'Integrated GPU', 'cpu': 'CPU'};
