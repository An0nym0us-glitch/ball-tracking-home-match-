// Independent ground-truth simulator (small fixed-step RK4 with explicit
// bounce/pad events) used to check the engine's fit.
#include "sim.hpp"

#include <cmath>

namespace pe::test {

Camera standardCamera(double L, int width, int height, double hfov_deg) {
  const double f = 0.5 * width / std::tan(0.5 * hfov_deg * kPi / 180.0);
  return Camera::lookAt(width, height, f, Vec3(0.05, -1.5, 1.25), Vec3(0.0, 0.62 * L, 0.25));
}

namespace {

struct State {
  double t;
  Vec3 p, v;
};

struct Physics {
  double k;
  double lateral;
};

Vec3 accel(const Vec3& v, const Physics& ph) {
  return Vec3(ph.lateral, 0.0, -kGravity) - ph.k * v.norm() * v;
}

void step(State& s, double h, const Physics& ph) {
  const Vec3 a1 = accel(s.v, ph);
  const Vec3 v2 = s.v + 0.5 * h * a1;
  const Vec3 a2 = accel(v2, ph);
  const Vec3 v3 = s.v + 0.5 * h * a2;
  const Vec3 a3 = accel(v3, ph);
  const Vec3 v4 = s.v + h * a3;
  const Vec3 a4 = accel(v4, ph);
  s.p += h / 6.0 * (s.v + 2 * v2 + 2 * v3 + v4);
  s.v += h / 6.0 * (a1 + 2 * a2 + 2 * a3 + a4);
  s.t += h;
}

struct Run {
  std::vector<State> states;  // dense samples (dt = 0.2 ms)
  bool bounced = false;
  State at_bounce{};
  bool padded = false;
  State at_pad{};
};

constexpr double kDt = 2e-4;

// Runs the delivery. `use_pad` = false gives the pad-free path for the stumps truth.
Run run(const SimDelivery& d, const Vec3& v0, const BallSpec& ball, double y_stop, bool use_pad,
        double t_after_pad = 0.35) {
  Run r;
  State s{d.t_release, d.release, v0};
  Physics ph{ball.drag_k, d.lateral_accel};
  r.states.push_back(s);
  const bool full_toss = std::isnan(d.bounce_y);
  double pad_t_end = 1e9;
  while (s.t < d.t_release + 3.0) {
    State prev = s;
    step(s, kDt, ph);
    if (!full_toss && !r.bounced && s.p.z() <= ball.radius) {
      const double a = (prev.p.z() - ball.radius) / (prev.p.z() - s.p.z());
      State b{prev.t + a * kDt, prev.p + a * (s.p - prev.p), prev.v + a * (s.v - prev.v)};
      b.p.z() = ball.radius;
      r.bounced = true;
      r.at_bounce = b;
      const double th = d.turn_deg * kPi / 180.0;
      const double vx = b.v.x(), vy = b.v.y();
      const double c = std::cos(th), sn = std::sin(th);
      // Positive turn rotates the heading (atan2(vx, vy)) towards +x.
      Vec3 vout(ball.friction * (vx * c + vy * sn), ball.friction * (-vx * sn + vy * c),
                -ball.restitution * b.v.z());
      s = State{b.t, b.p, vout};
      ph.lateral = 0.0;
    }
    if (use_pad && !r.padded && !std::isnan(d.pad_y) && s.p.y() >= d.pad_y) {
      r.padded = true;
      r.at_pad = s;
      s.v = Vec3(0.3 * s.v.x() + 0.4, -0.25 * s.v.y(), 0.4 * std::abs(s.v.z()) + 0.5);
      ph.lateral = 0.0;
      pad_t_end = s.t + t_after_pad;
    }
    // Later ground contacts (after the pad, or a second bounce) just reflect.
    if (s.p.z() < ball.radius && (r.bounced || r.padded) && s.v.z() < 0) s.v.z() = -0.5 * s.v.z();
    r.states.push_back(s);
    if (s.p.y() > y_stop || s.t > pad_t_end) break;
  }
  return r;
}

State interpolate(const std::vector<State>& st, double t) {
  for (size_t i = 1; i < st.size(); ++i) {
    if (st[i].t >= t) {
      const double a = (t - st[i - 1].t) / (st[i].t - st[i - 1].t);
      return {t, st[i - 1].p + a * (st[i].p - st[i - 1].p), st[i - 1].v + a * (st[i].v - st[i - 1].v)};
    }
  }
  return st.back();
}

std::optional<double> crossY(const std::vector<State>& st, double y) {
  for (size_t i = 1; i < st.size(); ++i) {
    if (st[i - 1].p.y() < y && st[i].p.y() >= y) {
      const double a = (y - st[i - 1].p.y()) / (st[i].p.y() - st[i - 1].p.y());
      return st[i - 1].t + a * (st[i].t - st[i - 1].t);
    }
  }
  return std::nullopt;
}

}  // namespace

SimOutput simulate(const SimDelivery& d, const Camera& cam, const Pitch& pitch, const BallSpec& ball,
                   std::mt19937& rng) {
  const double L = pitch.length;
  const bool full_toss = std::isnan(d.bounce_y);
  // Aim: adjust elevation and azimuth until the bounce (or the stumps height) matches.
  double elev = -0.05, azim = std::atan2(d.bounce_x - d.release.x(), (full_toss ? L : d.bounce_y) - d.release.y());
  auto velocity = [&](double e, double a) {
    return Vec3(d.speed * std::cos(e) * std::sin(a), d.speed * std::cos(e) * std::cos(a), d.speed * std::sin(e));
  };
  // Height (full toss) or bounce distance grows monotonically with elevation: bisect.
  auto miss = [&](double e, double a, double* err_x) {
    const Run r = run(d, velocity(e, a), ball, L + 2.0, false);
    if (full_toss) {
      const auto t = crossY(r.states, L);
      if (!t) return -1.0;
      const State s = interpolate(r.states, *t);
      *err_x = s.p.x() - d.bounce_x;
      return s.p.z() - d.full_toss_z_at_stumps;
    }
    if (!r.bounced) return 1.0;
    *err_x = r.at_bounce.p.x() - d.bounce_x;
    return r.at_bounce.p.y() - d.bounce_y;
  };
  for (int outer = 0; outer < 5; ++outer) {
    double lo = -0.8, hi = 0.6, err_x = 0;
    for (int it = 0; it < 40; ++it) {
      const double mid = 0.5 * (lo + hi);
      (miss(mid, azim, &err_x) > 0 ? hi : lo) = mid;
    }
    elev = 0.5 * (lo + hi);
    miss(elev, azim, &err_x);
    azim -= err_x / ((full_toss ? L : d.bounce_y) - d.release.y());
  }

  const Vec3 v0 = velocity(elev, azim);
  const Run free_run = run(d, v0, ball, L + 2.0, false);
  const Run seen = run(d, v0, ball, L + 1.5, true);

  SimOutput out;
  SimTruth& tr = out.truth;
  tr.release_speed = d.speed;
  tr.has_bounce = free_run.bounced;
  if (free_run.bounced) {
    tr.t_bounce = free_run.at_bounce.t;
    tr.bounce = free_run.at_bounce.p;
    const Vec3 vin = free_run.at_bounce.v;  // velocity just before the bounce
    // Turn as defined by the engine: heading after minus heading before.
    const State after = interpolate(free_run.states, tr.t_bounce + 3 * kDt);
    tr.turn_deg = (std::atan2(after.v.x(), after.v.y()) - std::atan2(vin.x(), vin.y())) * 180.0 / kPi;
  }
  if (const auto t = crossY(free_run.states, L)) tr.at_stumps = interpolate(free_run.states, *t).p;
  if (seen.padded) {
    tr.t_impact = seen.at_pad.t;
    tr.impact = seen.at_pad.p;
  }
  const Vec3 ref = tr.has_bounce ? tr.bounce : (seen.padded ? tr.impact : tr.at_stumps);
  tr.swing = ref.x() - (d.release.x() + v0.x() / v0.y() * (ref.y() - d.release.y()));

  std::normal_distribution<double> nd(0.0, 1.0);
  const double t_first = d.t_release + d.hidden_after_release_s;
  const double t_last = seen.states.back().t;
  for (int k = 0;; ++k) {
    const double t = std::ceil(t_first * d.fps) / d.fps + k / d.fps;
    if (t > t_last) break;
    const State s = interpolate(seen.states, t);
    if (cam.depth(s.p) < 0.5) continue;
    Observation o;
    o.t = t;
    o.uv = cam.project(s.p) + d.noise_px * Vec2(nd(rng), nd(rng));
    if (o.uv.x() < 0 || o.uv.y() < 0 || o.uv.x() >= cam.width || o.uv.y() >= cam.height) continue;
    const double rho = cam.apparentRadius(s.p, ball.radius);
    o.radius_px = std::max(0.5, rho * (1.0 + 0.1 * nd(rng)) + 0.3 * nd(rng));
    out.obs.push_back(o);
  }
  return out;
}

}  // namespace pe::test
