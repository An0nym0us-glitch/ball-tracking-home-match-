#pragma once

#include <atomic>
#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "pocketeye/config.hpp"

namespace pe {

struct VideoAnalysisOptions {
  std::string video_path;
  std::vector<double> timestamps;  // per-frame capture times; empty = from the file
  double fps_override = 0.0;       // > 0: frame_index / fps
  std::string overlay_path;        // non-empty: also write a replay video
  bool gray = false;               // detect on brightness only
  std::atomic<int>* progress_permille = nullptr;
  // Called after every frame (debugging hooks such as candidate dumps).
  std::function<void(int frame, double t, const Engine& engine)> on_frame;
};

// Runs the engine over a whole video file. Returns the result JSON (calibration,
// deliveries, per-step timings). Throws std::runtime_error on errors.
nlohmann::json analyzeVideoFile(const LoadedConfig& cfg, const VideoAnalysisOptions& opt,
                                std::vector<DeliveryResult>* results = nullptr);

// Synthetic speed test: renders `frames` frames with a moving ball and times the
// engine per frame. Used on first launch to suggest phone-only or laptop mode.
nlohmann::json runBenchmark(int width, int height, int frames);

}  // namespace pe
