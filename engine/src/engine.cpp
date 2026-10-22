#include "pocketeye/engine.hpp"

#include <algorithm>
#include <chrono>
#include <opencv2/imgproc.hpp>

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
      tracker_(cfg.analysis.camera.width, cfg.tracker) {
  if (cfg_.async_analysis) thread_ = std::thread(&Engine::worker, this);
}

Engine::~Engine() {
  if (thread_.joinable()) {
    {
      std::lock_guard<std::mutex> lock(mu_);
      stop_ = true;
    }
    cv_.notify_all();
    thread_.join();
  }
}

void Engine::pushFrame(const cv::Mat& image, double t) {
  toYuvFrame(image, converted_);
  pushFrame(converted_, t);
}

void Engine::pushFrame(const YuvFrame& frame, double t) {
  ++stats_.frames;
  t_now_ = t;
  const auto t0 = Clock::now();
  double t_mid = 0;
  const bool ready = detector_.push(frame, t, last_candidates_, t_mid);
  const double det_ms = msSince(t0);
  stats_.detect_ms += det_ms;
  ++frame_index_;
  if (ready) {
    const int mid_frame = frame_index_ - 2;
    detect_ms_per_frame_.push_back({mid_frame, det_ms});
    while (detect_ms_per_frame_.size() > 600) detect_ms_per_frame_.pop_front();
    const auto t1 = Clock::now();
    tracker_.update(mid_frame, t_mid, last_candidates_);
    stats_.track_ms += msSince(t1);
    queueFinished();
  }
  collectDone();
  emitPending(t, false);
}

void Engine::finish() {
  tracker_.flush();
  queueFinished();
  if (thread_.joinable()) {
    std::unique_lock<std::mutex> lock(mu_);
    cv_.wait(lock, [this] { return jobs_.empty() && running_ == 0; });
  }
  collectDone();
  emitPending(t_now_, true);
}

std::vector<Vec2> Engine::corridorOutline() const {
  std::vector<std::vector<cv::Point>> contours;
  cv::Mat mask = detector_.corridorMask().clone();
  cv::findContours(mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
  std::vector<Vec2> out;
  if (contours.empty()) return out;
  const auto& c = *std::max_element(contours.begin(), contours.end(),
                                    [](const auto& a, const auto& b) { return a.size() < b.size(); });
  for (const cv::Point& p : c) out.emplace_back(p.x, p.y);
  return out;
}

Engine::Stats Engine::stats() const {
  Stats s = stats_;
  std::lock_guard<std::mutex> lock(mu_);
  s.analyses_waiting = static_cast<int>(jobs_.size()) + running_;
  return s;
}

void Engine::queueFinished() {
  for (auto& track : tracker_.takeFinished(cfg_.min_track_points)) {
    Job job;
    for (const auto& [frame, d] : detect_ms_per_frame_) {
      if (frame >= track.front().frame && frame <= track.back().frame) job.detect_ms += d;
    }
    // The track closes some frames after the ball was last seen.
    job.close_frame = frame_index_ - 2;
    job.track = std::move(track);
    if (!thread_.joinable()) {
      collect(runJob(job));
      continue;
    }
    {
      std::lock_guard<std::mutex> lock(mu_);
      in_flight_.push_back({job.track.front().t, job.track.back().t});
      jobs_.push_back(std::move(job));
    }
    cv_.notify_all();
  }
}

void Engine::worker() {
  std::unique_lock<std::mutex> lock(mu_);
  while (true) {
    cv_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
    if (stop_) return;
    Job job = std::move(jobs_.front());
    jobs_.pop_front();
    ++running_;
    lock.unlock();
    DeliveryResult r = runJob(job);
    lock.lock();
    done_.push_back(std::move(r));
    const std::pair<double, double> span{job.track.front().t, job.track.back().t};
    const auto it = std::find(in_flight_.begin(), in_flight_.end(), span);
    if (it != in_flight_.end()) in_flight_.erase(it);
    --running_;
    cv_.notify_all();
  }
}

DeliveryResult Engine::runJob(const Job& job) const {
  const auto& track = job.track;
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
  r.timings_ms.insert(r.timings_ms.begin(), {"detect_frames_sum", job.detect_ms});
  r.timings_ms.push_back({"analysis_wall", msSince(t0)});
  r.timings_ms.push_back({"close_delay_frames", static_cast<double>(job.close_frame - r.frame_last)});
  return r;
}

void Engine::collectDone() {
  std::deque<DeliveryResult> done;
  {
    std::lock_guard<std::mutex> lock(mu_);
    done.swap(done_);
  }
  for (auto& r : done) collect(std::move(r));
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
  if (pending_.empty()) return;
  std::vector<std::pair<double, double>> in_flight;
  {
    std::lock_guard<std::mutex> lock(mu_);
    in_flight = in_flight_;
  }
  std::vector<DeliveryResult> keep;
  for (auto& r : pending_) {
    // A track still being analysed may be a duplicate of this delivery.
    bool waiting = false;
    for (const auto& [t0, t1] : in_flight) waiting |= t0 < r.t_last + cfg_.hold_s && t1 > r.t_first;
    if (all || (r.t_last + cfg_.hold_s < t_now && !waiting)) {
      ++stats_.deliveries;
      results_.push_back(std::move(r));
    } else {
      keep.push_back(std::move(r));
    }
  }
  pending_ = std::move(keep);
}

void Engine::collect(DeliveryResult r) {
  ++stats_.tracks_analyzed;
  for (const auto& [name, ms] : r.timings_ms) {
    if (name == "analysis_wall") stats_.analyze_ms += ms;
  }
  if (!r.valid) {
    ++stats_.rejected;
    stats_.last_reject = r.reject_reason;
    if (cfg_.keep_rejected) results_.push_back(std::move(r));
    return;
  }
  for (auto it = pending_.begin(); it != pending_.end();) {
    if (!overlaps(*it, r)) {
      ++it;
    } else if (better(*it, r)) {
      return;
    } else {
      it = pending_.erase(it);
    }
  }
  pending_.push_back(std::move(r));
}

bool Engine::popResult(DeliveryResult& out) {
  if (results_.empty() && thread_.joinable()) {
    collectDone();
    emitPending(t_now_, false);
  }
  if (results_.empty()) return false;
  out = std::move(results_.front());
  results_.pop_front();
  return true;
}

}  // namespace pe
