#include "pocketeye/tracker.hpp"

#include <Eigen/Dense>
#include <algorithm>
#include <cmath>

namespace pe {

void Kalman1D::predict(double dt, double q) {
  Eigen::Matrix3d F;
  F << 1, dt, 0.5 * dt * dt, 0, 1, dt, 0, 0, 1;
  const double d2 = dt * dt, d3 = d2 * dt, d4 = d3 * dt, d5 = d4 * dt;
  Eigen::Matrix3d Q;
  Q << d5 / 20, d4 / 8, d3 / 6, d4 / 8, d3 / 3, d2 / 2, d3 / 6, d2 / 2, dt;
  x = F * x;
  P = F * P * F.transpose() + q * Q;
}

void Kalman1D::update(double z, double r) {
  const double s = P(0, 0) + r;
  const Eigen::Vector3d k = P.col(0) / s;
  x += k * (z - x[0]);
  P -= k * P.row(0);
}

Tracker::Tracker(int image_width, TrackerConfig cfg) : width_(image_width), cfg_(cfg) {}

void Tracker::startFilter(TrackState& tr) {
  const TrackPoint& a = tr.points[tr.points.size() - 2];
  const TrackPoint& b = tr.points.back();
  const double dt = std::max(b.t - a.t, 1e-3);
  const double r = cfg_.meas_sigma_px * cfg_.meas_sigma_px;
  for (int axis = 0; axis < 2; ++axis) {
    Kalman1D& k = axis == 0 ? tr.ku : tr.kv;
    k.x << b.uv[axis], (b.uv[axis] - a.uv[axis]) / dt, 0.0;
    k.P.setZero();
    k.P(0, 0) = r;
    k.P(1, 1) = 2 * r / (dt * dt) + 100.0 * 100.0;
    k.P(2, 2) = 4000.0 * 4000.0;
  }
}

void Tracker::update(int frame, double t, const std::vector<Candidate>& cands) {
  struct Pair {
    double score;
    size_t track;
    size_t cand;
    bool rescue;
  };
  std::vector<Pair> pairs;
  std::vector<Kalman1D> pu(tracks_.size()), pv(tracks_.size());
  const double r = cfg_.meas_sigma_px * cfg_.meas_sigma_px;
  const double max_jump = cfg_.max_jump_frac * width_;

  for (size_t i = 0; i < tracks_.size(); ++i) {
    TrackState& tr = tracks_[i];
    const TrackPoint& last = tr.points.back();
    if (tr.points.size() >= 2) {
      pu[i] = tr.ku;
      pv[i] = tr.kv;
      const double dt = t - tr.t_last;
      pu[i].predict(dt, cfg_.jerk_density);
      pv[i].predict(dt, cfg_.jerk_density);
      const Vec2 pred(pu[i].x[0], pv[i].x[0]);
      const double su = pu[i].P(0, 0) + r, sv = pv[i].P(0, 0) + r;
      const double step = (pred - last.uv).norm();
      for (size_t j = 0; j < cands.size(); ++j) {
        const Vec2 e = cands[j].uv - pred;
        const double d2 = e.x() * e.x() / su + e.y() * e.y() / sv;
        if (d2 < cfg_.gate_sigma * cfg_.gate_sigma || e.norm() < cfg_.min_gate_px) {
          pairs.push_back({d2 - 1000.0 * std::min<size_t>(tr.points.size(), 50), i, j, false});
        } else if (e.norm() < std::max(cfg_.rescue_gate_px, 1.5 * step)) {
          pairs.push_back({e.norm(), i, j, true});
        }
      }
    } else {
      for (size_t j = 0; j < cands.size(); ++j) {
        const double d = (cands[j].uv - last.uv).norm();
        const double ratio = (cands[j].radius_px + 0.5) / (last.radius_px + 0.5);
        if (d > 0.5 && d < max_jump && ratio > 0.33 && ratio < 3.0) {
          pairs.push_back({1000.0 + d, i, j, false});
        }
      }
    }
  }
  std::sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) { return a.score < b.score; });

  std::vector<bool> track_used(tracks_.size(), false), cand_used(cands.size(), false);
  for (const Pair& p : pairs) {
    if (track_used[p.track] || cand_used[p.cand]) continue;
    track_used[p.track] = cand_used[p.cand] = true;
    TrackState& tr = tracks_[p.track];
    const Candidate& c = cands[p.cand];
    tr.points.push_back({frame, t, c.uv, c.radius_px, c.strength});
    tr.misses = 0;
    if (tr.points.size() == 2) {
      startFilter(tr);
    } else {
      tr.ku = pu[p.track];
      tr.kv = pv[p.track];
      if (p.rescue) {
        startFilter(tr);  // direction changed (bounce / impact): restart from the last two points
      } else {
        tr.ku.update(c.uv.x(), r);
        tr.kv.update(c.uv.y(), r);
      }
    }
    tr.t_last = t;
  }

  std::vector<TrackState> keep;
  for (size_t i = 0; i < tracks_.size(); ++i) {
    TrackState& tr = tracks_[i];
    if (!track_used[i]) ++tr.misses;
    const int allowed = tr.points.size() >= 3 ? cfg_.max_misses : 1;
    if (tr.misses > allowed) {
      if (tr.points.size() >= 3) finished_.push_back(std::move(tr.points));
    } else {
      keep.push_back(std::move(tr));
    }
  }
  tracks_ = std::move(keep);

  for (size_t j = 0; j < cands.size(); ++j) {
    if (cand_used[j]) continue;
    TrackState tr;
    tr.id = next_id_++;
    tr.points.push_back({frame, t, cands[j].uv, cands[j].radius_px, cands[j].strength});
    tr.t_last = t;
    tracks_.push_back(std::move(tr));
  }
  if (static_cast<int>(tracks_.size()) > cfg_.max_tracks) {
    std::stable_sort(tracks_.begin(), tracks_.end(), [](const TrackState& a, const TrackState& b) {
      return a.points.size() > b.points.size();
    });
    tracks_.resize(cfg_.max_tracks);
  }
}

void Tracker::flush() {
  for (TrackState& tr : tracks_) {
    if (tr.points.size() >= 3) finished_.push_back(std::move(tr.points));
  }
  tracks_.clear();
}

std::vector<std::vector<TrackPoint>> Tracker::takeFinished(size_t min_points) {
  std::vector<std::vector<TrackPoint>> out;
  for (auto& f : finished_) {
    if (f.size() >= min_points) out.push_back(std::move(f));
  }
  finished_.clear();
  return out;
}

}  // namespace pe
