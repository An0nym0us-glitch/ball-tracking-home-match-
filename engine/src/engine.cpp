#include "pocketeye/engine.hpp"

#include <algorithm>
#include <chrono>
#include <optional>
#include <opencv2/imgproc.hpp>

namespace pe {

namespace {
using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

#ifdef PE_WITH_AI
// Part of a camera frame (roi in picture pixels, even coordinates) as BGR,
// resized by `scale`, for the AI assist.
cv::Mat yuvToBgr(const YuvFrame& f, const cv::Rect& roi, double scale) {
  const cv::Size size(std::max(1, static_cast<int>(roi.width * scale)), std::max(1, static_cast<int>(roi.height * scale)));
  cv::Mat y;
  cv::resize(f.y(roi), y, size, 0, 0, scale < 1.0 ? cv::INTER_AREA : cv::INTER_LINEAR);
  cv::Mat bgr;
  if (f.uv.empty()) {
    cv::cvtColor(y, bgr, cv::COLOR_GRAY2BGR);
    return bgr;
  }
  const cv::Rect uv_roi(roi.x / 2, roi.y / 2, roi.width / 2, roi.height / 2);
  cv::Mat uv;
  cv::resize(f.uv(uv_roi & cv::Rect(0, 0, f.uv.cols, f.uv.rows)), uv, size, 0, 0, cv::INTER_LINEAR);
  cv::Mat ch[2];
  cv::split(uv, ch);
  cv::Mat yuv;
  cv::merge(std::vector<cv::Mat>{y, ch[0], ch[1]}, yuv);
  cv::cvtColor(yuv, bgr, cv::COLOR_YUV2BGR);
  return bgr;
}

// Where a picture point lies on the ground (z = 0), if the ray goes down.
std::optional<Vec3> onGround(const Camera& cam, const Vec2& uv) {
  const Vec3 c = cam.center(), d = cam.rayDirection(uv);
  if (d.z() > -1e-6) return std::nullopt;
  return c + (-c.z() / d.z()) * d;
}
#endif
}  // namespace

Engine::Engine(const EngineConfig& cfg)
    : cfg_(cfg),
      detector_(cfg.analysis.camera, cfg.analysis.pitch, cfg.analysis.ball.radius, cfg.detector),
      tracker_(cfg.analysis.camera.width, cfg.tracker),
      follower_(cfg.analysis.camera.width,
                cfg.analysis.camera.project(Vec3(0.0, cfg.analysis.pitch.length, 0.5 * cfg.analysis.pitch.stump_height))) {
  if (cfg_.ai.enabled) {
#ifdef PE_WITH_AI
    ai_ = std::make_unique<AiAssist>(cfg_.ai);
#else
    stats_.ai_error = "this build has no AI assist";
#endif
  }
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
    follower_.update(t_mid, last_candidates_);
    if (ai_) aiStep(frame, t);
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

void Engine::aiStep(const YuvFrame& frame, double t) {
#ifdef PE_WITH_AI
  const AiStatus st = ai_->status();
  follower_.enableChecks(st.ready);
  if (!st.ready) return;
  const int W = frame.y.cols, H = frame.y.rows;
  if (ai_->wantsPeople(t)) ai_->submitPeople(t, yuvToBgr(frame, cv::Rect(0, 0, W & ~1, H & ~1), 0.5), 2.0);
  // Ball checks: a crop around where the spot is now, about 4.5 ball widths
  // across (the model finds a ball best when it is 6-24 px in a 104 px crop).
  for (const LiveFollower::CheckRequest& rq : follower_.takeCheckRequests()) {
    const int want = std::max(cfg_.ai.crop_px, static_cast<int>(9.0 * rq.radius_px));
    const int c = std::min({want & ~1, W & ~1, H & ~1});
    const Vec2 uv = rq.uv + rq.v * std::clamp(t - rq.t, 0.0, 0.2);
    const int x0 = std::clamp(static_cast<int>(std::lround(uv.x() - c / 2.0)) & ~1, 0, (W - c) & ~1);
    const int y0 = std::clamp(static_cast<int>(std::lround(uv.y() - c / 2.0)) & ~1, 0, (H - c) & ~1);
    const cv::Rect roi(x0, y0, c, c);
    ai_->submitCheck({rq.id, t, Vec2(x0 + c / 2.0, y0 + c / 2.0)}, yuvToBgr(frame, roi, 1.0));
  }
  for (const AiCheckResult& r : ai_->takeChecks()) {
    follower_.applyCheck(r.id, r.t, r.score >= cfg_.ai.ball_score, r.uv);
  }
  // People: a moving spot inside one needs a ball check before it is followed.
  // The bowler (feet in the near half of the pitch) and a batter (feet near
  // the batter's stumps) are told apart by where they stand on the ground.
  std::vector<AiBox> boxes;
  double tp = 0;
  if (ai_->people(boxes, tp) && tp > people_t_) {
    people_t_ = tp;
    const Camera& cam = cfg_.analysis.camera;
    const double L = cfg_.analysis.pitch.length;
    std::vector<Person> people;
    std::vector<cv::Rect2f> areas;
    bool batter = false;
    for (const AiBox& b : boxes) {
      Person p{b.box, '?'};
      if (const auto g = onGround(cam, Vec2(b.box.x + b.box.width / 2, b.box.y + b.box.height))) {
        if (g->y() > -6.0 && g->y() < 0.45 * L && std::abs(g->x()) < 4.0) p.role = 'b';
        if (g->y() > L - 3.5 && g->y() < L + 1.5 && std::abs(g->x()) < 3.0) p.role = 'B';
      }
      // A little larger: the arm swings, the box lags behind.
      const float mx = 0.15f * b.box.width, my = 0.1f * b.box.height;
      areas.emplace_back(b.box.x - mx, b.box.y - my, b.box.width + 2 * mx, b.box.height + 2 * my);
      batter |= p.role == 'B';
      people.push_back(p);
    }
    follower_.setPeople(std::move(areas), tp + 0.6);
    people_ = std::move(people);
    batter_seen_.push_back({tp, batter});
    while (!batter_seen_.empty() && batter_seen_.front().first < tp - 30.0) batter_seen_.pop_front();
  }
#else
  (void)frame;
  (void)t;
#endif
}

Engine::Stats Engine::stats() const {
  Stats s = stats_;
#ifdef PE_WITH_AI
  if (ai_) {
    const AiStatus st = ai_->status();
    s.ai_device = st.ready ? st.device : "";
    s.ai_note = st.note;
    s.ai_error = st.error;
    s.ai_person_ms = st.person_ms;
    s.ai_check_ms = st.check_ms;
    s.people = static_cast<int>(people_.size());
    s.batter_seen = batter_seen_.empty() ? -1 : (batter_seen_.back().second ? 1 : 0);
  }
#endif
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
    job.batter_present = cfg_.analysis.batter_present;
    if (cfg_.batter_auto && job.batter_present) {
      // A batter seen at the stumps around this delivery? No AI answer: keep the setting.
      bool any = false, seen = false;
      for (const auto& [ts, b] : batter_seen_) {
        if (ts < track.front().t - 1.0 || ts > track.back().t + 0.3) continue;
        any = true;
        seen |= b;
      }
      if (any) job.batter_present = seen;
    }
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
    AnalysisConfig a = cfg_.analysis;
    a.batter_present = job.batter_present;
    r = analyzeTrack(track, a);
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
// True if a's flight (before the impact) passes through b's first point: b
// is then the rest of the same ball (after the bounce, or rolling away after
// the pad) taken for a delivery of its own.
bool continues(const DeliveryResult& a, const DeliveryResult& b) {
  if (b.observations.empty() || !(a.t_first < b.t_first)) return false;
  const Observation& first = b.observations.front();
  for (size_t i = 0; i < a.observations.size() && i < a.labels.size(); ++i) {
    const Observation& o = a.observations[i];
    if ((a.labels[i] == kPreBounce || a.labels[i] == kPostBounce) && std::abs(o.t - first.t) < 1e-6 &&
        (o.uv - first.uv).norm() < 2.0) {
      return true;
    }
  }
  return false;
}

// Several tracks can follow the same ball (one may have started on noise).
// Prefer the one that the other continues, then the one that explains more of
// the flight, then the cleaner fit.
bool better(const DeliveryResult& a, const DeliveryResult& b) {
  if (continues(a, b)) return true;
  if (continues(b, a)) return false;
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
