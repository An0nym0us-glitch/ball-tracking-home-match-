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

bool LiveFollower::inPerson(const Vec2& uv, double t) const {
  if (t > people_until_) return false;
  for (const cv::Rect2f& r : people_) {
    if (uv.x() >= r.x && uv.x() <= r.x + r.width && uv.y() >= r.y && uv.y() <= r.y + r.height) return true;
  }
  return false;
}

void LiveFollower::request(Track& tr, double t) {
  tr.check = 1;
  ++tr.checks;
  tr.hits_at_check = tr.hits;
  requests_.push_back({tr.id, t, tr.p, tr.v, tr.r});
}

std::vector<LiveFollower::CheckRequest> LiveFollower::takeCheckRequests() {
  std::vector<CheckRequest> out;
  out.swap(requests_);
  return out;
}

void LiveFollower::applyCheck(int id, double t, bool is_ball, const Vec2& uv) {
  if (id == recover_id_) {
    recover_id_ = 0;
    // The lost ball found again: continue from there.
    if (is_ball && active_ && t > ball_.t) {
      Candidate c;
      c.uv = uv;
      c.radius_px = ball_.r;
      advance(ball_, t, c);
    }
    return;
  }
  for (Track& tr : tentative_) {
    if (tr.id == id && tr.check == 1) tr.check = is_ball ? 2 : 3;
  }
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
      if (checks_on_ && recover_id_ == 0) {
        // Not seen moving (it may hardly move in the picture): look where it should be.
        recover_id_ = next_id_++;
        requests_.push_back({recover_id_, ball_.t, ball_.p, ball_.v, ball_.r});
      }
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
    bool confirm = tr.hits >= kConfirmHits && (!active_ || ball_.misses > 0) && headsForTarget(tr);
    if (confirm && checks_on_ && inPerson(tr.p, t)) {
      // Inside a person: a second opinion first. A spot judged "not a ball" (a
      // hand, a shoe) gets one more chance a few frames later (a blurred ball
      // can fail once, e.g. at the release).
      if (tr.check == 0 || (tr.check == 3 && tr.checks < 2 && tr.hits - tr.hits_at_check >= 6)) request(tr, t);
      confirm = tr.check == 2;
    }
    if (confirm) {
      ball_ = tr;
      active_ = true;
      recover_id_ = 0;
    } else {
      next.push_back(tr);
    }
  }
  // New starts from the strongest unused candidates (they come strongest first).
  for (size_t i = 0; i < cands.size() && next.size() < 8; ++i) {
    if (used[i]) continue;
    Track tr;
    tr.id = next_id_++;
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
