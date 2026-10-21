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

void Engine::pushFrame(const cv::Mat& image, double t) {
  ++stats_.frames;
  t_now_ = t;
  const auto t0 = Clock::now();
  double t_mid = 0;
  const bool ready = detector_.push(image, t, last_candidates_, t_mid);
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
  emitPending(t, false);
}

void Engine::finish() {
  tracker_.flush();
  analyzeFinished();
  emitPending(t_now_, true);
}

namespace {
// Several tracks can follow the same ball (one may have started on noise).
// Prefer the one that explains more of the flight, then the cleaner fit.
bool better(const DeliveryResult& a, const DeliveryResult& b) {
  const int na = a.n_pre + a.n_post, nb = b.n_pre + b.n_post;
  if (na != nb) return na > nb;
  return a.rms_px < b.rms_px;
}

bool overlaps(const DeliveryResult& a, const DeliveryResult& b) {
  const double lo = std::max(a.t_first, b.t_first), hi = std::min(a.t_last, b.t_last);
  const double shorter = std::min(a.t_last - a.t_first, b.t_last - b.t_first);
  return hi - lo > 0.3 * shorter;
}
}  // namespace

void Engine::emitPending(double t_now, bool all) {
  std::vector<DeliveryResult> keep;
  for (auto& r : pending_) {
    if (all || r.t_last + cfg_.hold_s < t_now) {
      ++stats_.deliveries;
      results_.push_back(std::move(r));
    } else {
      keep.push_back(std::move(r));
    }
  }
  pending_ = std::move(keep);
}

void Engine::analyzeFinished() {
  for (const auto& track : tracker_.takeFinished(cfg_.min_track_points)) {
    const auto t0 = Clock::now();
    DeliveryResult r;
    if (cfg_.skip_analysis) {
      for (const TrackPoint& p : track) r.observations.push_back({p.t, p.uv, p.radius_px});
      r.labels.assign(track.size(), 0);
      r.frame_first = track.front().frame;
      r.frame_last = track.back().frame;
      r.t_first = track.front().t;
      r.t_last = track.back().t;
      r.reject_reason = "not analysed";
    } else {
      r = analyzeTrack(track, cfg_.analysis);
    }
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
      bool dropped = false;
      for (auto it = pending_.begin(); it != pending_.end();) {
        if (!overlaps(*it, r)) {
          ++it;
        } else if (better(*it, r)) {
          dropped = true;
          break;
        } else {
          it = pending_.erase(it);
        }
      }
      if (!dropped) pending_.push_back(std::move(r));
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
