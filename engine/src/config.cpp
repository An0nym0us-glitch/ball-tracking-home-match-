#include <algorithm>
#include "pocketeye/config.hpp"

#include <stdexcept>

namespace pe {

using nlohmann::json;

json cameraToJson(const Camera& c) {
  json R = json::array();
  for (int i = 0; i < 3; ++i) R.push_back({c.R(i, 0), c.R(i, 1), c.R(i, 2)});
  return {{"width", c.width}, {"height", c.height}, {"f", c.f},  {"cx", c.cx},
          {"cy", c.cy},       {"R", R},             {"t", {c.t.x(), c.t.y(), c.t.z()}}};
}

Camera cameraFromJson(const json& j) {
  Camera c;
  c.width = j.at("width").get<int>();
  c.height = j.at("height").get<int>();
  c.f = j.at("f").get<double>();
  c.cx = j.value("cx", 0.5 * c.width);
  c.cy = j.value("cy", 0.5 * c.height);
  for (int i = 0; i < 3; ++i) {
    for (int k = 0; k < 3; ++k) c.R(i, k) = j.at("R").at(i).at(k).get<double>();
    c.t[i] = j.at("t").at(i).get<double>();
  }
  return c;
}

json calibrationToJson(const CalibrationResult& c) {
  const Vec3 ctr = c.camera.center();
  return {{"ok", c.ok},
          {"error", c.error},
          {"camera", cameraToJson(c.camera)},
          {"camera_position_m", {ctr.x(), ctr.y(), ctr.z()}},
          {"hfov_deg", 2.0 * std::atan(0.5 * c.camera.width / c.camera.f) * 180.0 / kPi},
          {"pitch_length_m", c.pitch_length},
          {"pitch_length_sigma_m", c.pitch_length_sigma},
          {"rms_px", c.rms_px},
          {"max_px", c.max_px},
          {"warnings", c.warnings}};
}

CalibrationInput calibrationInputFromJson(const json& j, bool with_image) {
  CalibrationInput in;
  in.length_known = false;
  if (j.contains("pitch")) {
    const json& p = j["pitch"];
    if (p.contains("length_m") && !p["length_m"].is_null()) {
      in.pitch.length = p["length_m"].get<double>();
      in.length_known = true;
    } else {
      in.pitch.length = 18.0;
    }
    in.pitch.width = p.value("width_m", in.pitch.width);
    in.pitch.stump_height = p.value("stump_height_m", in.pitch.stump_height);
    in.pitch.stump_width = p.value("stump_width_m", in.pitch.stump_width);
    in.pitch.stump_diameter = p.value("stump_diameter_m", in.pitch.stump_diameter);
  } else {
    in.pitch.length = 18.0;
  }
  in.bowler_stumps = j.value("bowler_stumps", true);
  if (!with_image) return in;
  in.width = j.at("image").at("width").get<int>();
  in.height = j.at("image").at("height").get<int>();
  if (j.contains("camera_hint") && j["camera_hint"].contains("hfov_deg")) {
    in.hfov_deg = j["camera_hint"]["hfov_deg"].get<double>();
    if (j["camera_hint"].contains("hfov_sigma_deg")) in.hfov_sigma_deg = j["camera_hint"]["hfov_sigma_deg"].get<double>();
  }
  if (j.contains("camera_height_m") && !j["camera_height_m"].is_null()) {
    in.camera_height_m = j["camera_height_m"].get<double>();
  }
  if (j.contains("camera_distance_m") && !j["camera_distance_m"].is_null()) {
    in.camera_distance_m = j["camera_distance_m"].get<double>();
  }
  if (j.contains("taps")) {
    for (const auto& [name, uv] : j["taps"].items()) {
      in.taps.push_back({name, Vec2(uv.at(0).get<double>(), uv.at(1).get<double>())});
    }
  }
  return in;
}

LoadedConfig loadConfig(const json& j) {
  LoadedConfig out;
  AnalysisConfig& a = out.engine.analysis;

  const std::string ball = j.value("ball", "tennis");
  const auto kind = parseBallKind(ball);
  if (!kind) throw std::runtime_error("unknown ball type: " + ball);
  a.ball = ballSpec(*kind);
  const std::string batter = j.value("batter", "right");
  if (batter != "right" && batter != "left" && batter != "none") {
    throw std::runtime_error("batter must be right, left or none");
  }
  a.batter = batter == "left" ? Hand::Left : Hand::Right;  // "none": sides as for a right-hander
  a.batter_present = batter != "none";
  a.no_shot_offered = j.value("no_shot_offered", false);
  a.release_offset_m = j.value("release_offset_m", 0.8);
  a.sigma_px = j.value("sigma_px", 1.0);
  a.mc_samples = j.value("mc_samples", 300);
  // How sure a definite LBW / stumps verdict must be; below that it is umpire's call.
  a.decision_probability = std::clamp(j.value("decision_probability", 0.9), 0.6, 0.99);
  // "jpeg_stream": every frame arrives as its own JPEG (old-phone camera app).
  const std::string source = j.value("source", "camera");
  if (source != "camera" && source != "jpeg_stream") {
    throw std::runtime_error("source must be camera or jpeg_stream");
  }
  if (source == "jpeg_stream") out.engine.detector = forJpegStream(out.engine.detector);
  // YOLO assist: {"model": "yolox_tiny.onnx", "device": "dgpu" | "igpu" | "cpu"}.
  if (j.contains("ai") && j["ai"].is_object() && !j["ai"].value("model", "").empty()) {
    AiConfig& ai = out.engine.ai;
    ai.enabled = true;
    ai.model = j["ai"]["model"].get<std::string>();
    ai.device = j["ai"].value("device", ai.device);
    if (ai.device != "dgpu" && ai.device != "igpu" && ai.device != "cpu") {
      throw std::runtime_error("ai.device must be dgpu, igpu or cpu");
    }
    ai.ball_score = j["ai"].value("ball_score", ai.ball_score);
    ai.person_hz = j["ai"].value("person_hz", ai.person_hz);
  }
  out.engine.batter_auto = j.value("batter_auto", false);

  CalibrationInput in = calibrationInputFromJson(j, false);
  a.pitch = in.pitch;

  if (j.contains("camera")) {
    a.camera = cameraFromJson(j["camera"]);
    if (!in.length_known) throw std::runtime_error("pitch.length_m is required with a solved camera");
    out.calibration.ok = true;
    out.calibration.camera = a.camera;
    out.calibration.pitch_length = a.pitch.length;
    return out;
  }

  if (!j.contains("taps")) throw std::runtime_error("config needs 'taps' (or a solved 'camera')");
  in = calibrationInputFromJson(j, true);
  out.calibration = calibrateFromTaps(in);
  if (!out.calibration.ok) throw std::runtime_error("calibration failed: " + out.calibration.error);
  a.camera = out.calibration.camera;
  a.pitch.length = out.calibration.pitch_length;
  return out;
}

}  // namespace pe
