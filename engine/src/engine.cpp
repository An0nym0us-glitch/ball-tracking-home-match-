#include "pocketeye/engine.hpp"

#include <chrono>

namespace pe {

namespace {
using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}
}  // namespace

Engine::Engine(const EngineConfig& cfg)
    : cfg_(cfg),
      detector_(cfg.analysis.camera, cfg.analysis.pitch, cfg.analysis.ball.radius, cfg.detector),
      tracker_(cfg.analysis.camera.width, cfg.tracker) {}

void Engine::pushFrame(const cv::Mat& gray, double t) {
  ++stats_.frames;
  const auto t0 = Clock::now();
  double t_mid = 0;
  const bool ready = detector_.push(gray, t, last_candidates_, t_mid);
  const double det_ms = msSince(t0);
  stats_.detect_ms += det_ms;
  ++frame_index_;
  if (!ready) return;
  const int mid_frame = frame_index_ - 2;
  detect_ms_per_frame_.push_back({mid_frame, det_ms});
  while (detect_ms_per_frame_.size() > 600) detect_ms_per_frame_.pop_front();

  const auto t1 = Clock::now();
  tracker_.update(mid_frame, t_mid, last_candidates_);
  stats_.track_ms += msSince(t1);
  analyzeFinished();
}

void Engine::finish() {
  tracker_.flush();
  analyzeFinished();
}

void Engine::analyzeFinished() {
  for (const auto& track : tracker_.takeFinished(cfg_.min_track_points)) {
    const auto t0 = Clock::now();
    DeliveryResult r = analyzeTrack(track, cfg_.analysis);
    const double ms = msSince(t0);
    stats_.analyze_ms += ms;
    ++stats_.tracks_analyzed;
    double det = 0;
    for (const auto& [frame, d] : detect_ms_per_frame_) {
      if (frame >= r.frame_first && frame <= r.frame_last) det += d;
    }
    r.timings_ms.insert(r.timings_ms.begin(), {"detect_frames_sum", det});
    // Wall time of the analysis once the track closed; the track closes
    // `max_misses` frames after the ball was last seen.
    r.timings_ms.push_back({"analysis_wall", ms});
    r.timings_ms.push_back({"close_delay_frames", static_cast<double>(frame_index_ - 2 - r.frame_last)});
    if (r.valid) {
      ++stats_.deliveries;
      results_.push_back(std::move(r));
    } else if (cfg_.keep_rejected) {
      results_.push_back(std::move(r));
    }
  }
}

bool Engine::popResult(DeliveryResult& out) {
  if (results_.empty()) return false;
  out = std::move(results_.front());
  results_.pop_front();
  return true;
}

}  // namespace pe
