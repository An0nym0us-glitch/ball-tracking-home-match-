#include "pocketeye/live.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace pe {

int LiveFollower::match(const Track& tr, double t, const std::vector<Candidate>& cands,
                        const std::vector<bool>& used, bool confirmed) const {
  const double dt = t - tr.t;
  if (dt <= 0) return -1;
  const double frames = std::max(1.0, dt * 30.0);
  Vec2 pred = tr.p;
  double gate = 0;
  if (tr.hits >= 2) {
    pred = tr.p + tr.v * dt;
    const double step = tr.v.norm() * dt;
    // A followed ball may bounce (the motion turns sharply); one still to be
    // confirmed must move steadily, which random flicker almost never does.
    gate = confirmed ? 8.0 + 2.0 * tr.r + 0.6 * step : 4.0 + 1.5 * tr.r + 0.15 * step;
  } else {
    gate = 0.1 * image_width_ * frames;  // no velocity yet: any plausible step
  }
  int best = -1;
  double best_d = std::numeric_limits<double>::infinity();
  for (size_t i = 0; i < cands.size(); ++i) {
    if (used[i]) continue;
    const double d = (cands[i].uv - pred).norm();
    // The ball always moves: a candidate on the same spot is flicker.
    if (d > gate || (cands[i].uv - tr.p).norm() < 1.5) continue;
    if (d < best_d) {
      best_d = d;
      best = static_cast<int>(i);
    }
  }
  return best;
}

void LiveFollower::advance(Track& tr, double t, const Candidate& c) {
  const double dt = t - tr.t;
  if (tr.hits >= 1 && dt > 0) {
    const Vec2 v = (c.uv - tr.p) / dt;
    tr.v = tr.hits >= 2 ? Vec2(0.4 * tr.v + 0.6 * v) : v;
  }
  tr.p = c.uv;
  tr.r = c.radius_px;
  tr.t = t;
  ++tr.hits;
  tr.misses = 0;
  tr.trail.push_back(c.uv);
  while (tr.trail.size() > kTrail) tr.trail.pop_front();
}

bool LiveFollower::headsForTarget(const Track& tr) const {
  if (!std::isfinite(target_.x())) return true;
  const Vec2 to = target_ - tr.p;
  if (to.norm() < 0.05 * image_width_) return true;  // already there: any direction (bounce, rebound)
  const double sp = tr.v.norm();
  return sp > 0 && tr.v.dot(to) / (sp * to.norm()) > 0.5;  // within 60 degrees
}

void LiveFollower::update(double t, const std::vector<Candidate>& cands) {
  std::vector<bool> used(cands.size(), false);
  if (active_) {
    const int k = match(ball_, t, cands, used, true);
    if (k >= 0) {
      advance(ball_, t, cands[k]);
      used[k] = true;
    } else if (++ball_.misses > kMaxMisses) {
      active_ = false;
    } else {
      // Carry the ball forward so the next prediction starts from where it should be.
      const double dt = t - ball_.t;
      ball_.p += ball_.v * dt;
      ball_.t = t;
      ball_.trail.push_back(ball_.p);
      while (ball_.trail.size() > kTrail) ball_.trail.pop_front();
    }
  }
  std::vector<Track> next;
  for (Track& tr : tentative_) {
    const int k = match(tr, t, cands, used, false);
    if (k < 0) continue;  // a candidate that is not followed up is dropped
    advance(tr, t, cands[k]);
    used[k] = true;
    if (tr.hits >= kConfirmHits && (!active_ || ball_.misses > 0) && headsForTarget(tr)) {
      ball_ = tr;
      active_ = true;
    } else {
      next.push_back(tr);
    }
  }
  // New starts from the strongest unused candidates (they come strongest first).
  for (size_t i = 0; i < cands.size() && next.size() < 8; ++i) {
    if (used[i]) continue;
    Track tr;
    advance(tr, t, cands[i]);
    next.push_back(tr);
  }
  tentative_ = std::move(next);
}

LiveBall LiveFollower::state(double t_now) const {
  LiveBall b;
  if (!active_) return b;
  b.visible = true;
  b.t = std::max(t_now, ball_.t);
  const double ahead = std::min(b.t - ball_.t, 0.1);
  b.uv = ball_.p + ball_.v * ahead;
  b.predicted = ball_.misses > 0 || ahead > 0;
  b.velocity = ball_.v;
  b.radius_px = ball_.r;
  b.trail.assign(ball_.trail.begin(), ball_.trail.end());
  if (ahead > 0) b.trail.push_back(b.uv);
  return b;
}

}  // namespace pe
