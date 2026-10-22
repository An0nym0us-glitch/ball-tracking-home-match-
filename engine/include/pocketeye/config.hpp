#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "pocketeye/calibration.hpp"
#include "pocketeye/engine.hpp"

namespace pe {

// Session configuration (JSON). Example:
// {
//   "image":  {"width": 1280, "height": 720},
//   "pitch":  {"length_m": 16.0, "stump_height_m": 0.66, "stump_width_m": 0.21},
//   "taps":   {"batter_left_base": [612, 402], "batter_left_top": [612, 371], ...},
//   "camera_hint": {"hfov_deg": 63},
//   "camera_height_m": 1.2,     (optional: measured lens height and distance behind the
//   "camera_distance_m": 1.5,    bowler's stumps; needed if those stumps are out of frame)
//   "ball": "tennis", "batter": "right", "no_shot_offered": false
// }
// "length_m" may be omitted: it is then estimated from the stumps.
// A solved "camera" block may be given instead of taps.
struct LoadedConfig {
  EngineConfig engine;
  CalibrationResult calibration;
};

LoadedConfig loadConfig(const nlohmann::json& j);  // throws std::runtime_error
// Image size, pitch and stump sizes, camera hints and measurements, taps (if any).
// with_image = false: only the pitch block is read.
CalibrationInput calibrationInputFromJson(const nlohmann::json& j, bool with_image = true);

nlohmann::json cameraToJson(const Camera& cam);
Camera cameraFromJson(const nlohmann::json& j);
nlohmann::json calibrationToJson(const CalibrationResult& c);

}  // namespace pe
