#include "pocketeye/flight.hpp"

#include <algorithm>
#include <cmath>

namespace pe {

Vec3 flightAcceleration(const Vec3& v, const FlightParams& fp) {
  return Vec3(fp.lateral_accel, 0.0, -kGravity) - fp.drag_k * v.norm() * v;
}

namespace {

void rk4(Vec3& p, Vec3& v, double h, const FlightParams& fp) {
  const Vec3 a1 = flightAcceleration(v, fp);
  const Vec3 v2 = v + 0.5 * h * a1;
  const Vec3 a2 = flightAcceleration(v2, fp);
  const Vec3 v3 = v + 0.5 * h * a2;
  const Vec3 a3 = flightAcceleration(v3, fp);
  const Vec3 v4 = v + h * a3;
  const Vec3 a4 = flightAcceleration(v4, fp);
  p += h / 6.0 * (v + 2.0 * v2 + 2.0 * v3 + v4);
  v += h / 6.0 * (a1 + 2.0 * a2 + 2.0 * a3 + a4);
}

}  // namespace

FlightPath::FlightPath(const Vec3& p0, const Vec3& v0, double t0, double t_begin, double t_end,
                       const FlightParams& fp, double max_step)
    : fp_(fp) {
  t_begin = std::min(t_begin, t0);
  t_end = std::max(t_end, t0);
  const int nb = std::max(1, static_cast<int>(std::ceil((t0 - t_begin) / max_step)));
  const int nf = std::max(1, static_cast<int>(std::ceil((t_end - t0) / max_step)));
  const double hb = (t0 - t_begin) / nb;
  const double hf = (t_end - t0) / nf;

  std::vector<double> bt;
  std::vector<Vec3> bp, bv;
  Vec3 p = p0, v = v0;
  if (hb > 0) {
    for (int i = 1; i <= nb; ++i) {
      rk4(p, v, -hb, fp);
      bt.push_back(t0 - i * hb);
      bp.push_back(p);
      bv.push_back(v);
    }
  }
  ts_.reserve(bt.size() + nf + 1);
  for (int i = static_cast<int>(bt.size()) - 1; i >= 0; --i) {
    ts_.push_back(bt[i]);
    ps_.push_back(bp[i]);
    vs_.push_back(bv[i]);
  }
  ts_.push_back(t0);
  ps_.push_back(p0);
  vs_.push_back(v0);
  if (hf > 0) {
    p = p0;
    v = v0;
    for (int i = 1; i <= nf; ++i) {
      rk4(p, v, hf, fp);
      ts_.push_back(t0 + i * hf);
      ps_.push_back(p);
      vs_.push_back(v);
    }
  }
}

void FlightPath::evaluate(double t, Vec3* p, Vec3* v) const {
  if (ts_.size() < 2) {
    if (p) *p = ps_.empty() ? Vec3::Zero() : ps_[0];
    if (v) *v = vs_.empty() ? Vec3::Zero() : vs_[0];
    return;
  }
  // Outside the integrated range: second-order Taylor step from the end node.
  if (t < ts_.front() || t > ts_.back()) {
    const size_t i = t < ts_.front() ? 0 : ts_.size() - 1;
    const double dt = t - ts_[i];
    const Vec3 a = flightAcceleration(vs_[i], fp_);
    if (p) *p = ps_[i] + vs_[i] * dt + 0.5 * a * dt * dt;
    if (v) *v = vs_[i] + a * dt;
    return;
  }
  size_t i = static_cast<size_t>(std::upper_bound(ts_.begin(), ts_.end(), t) - ts_.begin());
  i = std::clamp<size_t>(i, 1, ts_.size() - 1) - 1;
  const double h = ts_[i + 1] - ts_[i];
  const double s = h > 0 ? (t - ts_[i]) / h : 0.0;
  const double s2 = s * s, s3 = s2 * s;
  if (p) {
    *p = (2 * s3 - 3 * s2 + 1) * ps_[i] + (s3 - 2 * s2 + s) * h * vs_[i] +
         (-2 * s3 + 3 * s2) * ps_[i + 1] + (s3 - s2) * h * vs_[i + 1];
  }
  if (v) {
    *v = ((6 * s2 - 6 * s) * ps_[i] + (3 * s2 - 4 * s + 1) * h * vs_[i] +
          (-6 * s2 + 6 * s) * ps_[i + 1] + (3 * s2 - 2 * s) * h * vs_[i + 1]) /
         h;
  }
}

Vec3 FlightPath::position(double t) const {
  Vec3 p;
  evaluate(t, &p, nullptr);
  return p;
}

Vec3 FlightPath::velocity(double t) const {
  Vec3 v;
  evaluate(t, nullptr, &v);
  return v;
}

std::optional<double> FlightPath::timeAtY(double y_target, double t_from, double t_to) const {
  if (ts_.empty() || t_to <= t_from) return std::nullopt;
  // Coarse scan on 2 ms steps, then bisection.
  const double step = 0.002;
  double ta = t_from;
  double ya = position(ta).y() - y_target;
  if (ya == 0.0) return ta;
  while (ta < t_to) {
    const double tb = std::min(ta + step, t_to);
    const double yb = position(tb).y() - y_target;
    if ((ya < 0) != (yb < 0) || yb == 0.0) {
      double lo = ta, hi = tb, flo = ya;
      for (int k = 0; k < 40; ++k) {
        const double mid = 0.5 * (lo + hi);
        const double fm = position(mid).y() - y_target;
        if ((fm < 0) == (flo < 0)) {
          lo = mid;
          flo = fm;
        } else {
          hi = mid;
        }
      }
      return 0.5 * (lo + hi);
    }
    ta = tb;
    ya = yb;
  }
  return std::nullopt;
}

std::optional<double> Trajectory::timeAtY(double y_target, double t_from) const {
  if (!has_bounce) return pre.timeAtY(y_target, t_from, pre.tEnd());
  if (t_from < t_bounce) {
    if (auto t = pre.timeAtY(y_target, t_from, t_bounce)) return t;
    t_from = t_bounce;
  }
  return post.timeAtY(y_target, t_from, post.tEnd());
}

}  // namespace pe
