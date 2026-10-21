#include "pocketeye/analysis.hpp"

#include <Eigen/Dense>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <random>
#include <sstream>

namespace pe {

const char* impactKindName(ImpactKind k) {
  switch (k) {
    case ImpactKind::None: return "none";
    case ImpactKind::Pad: return "pad";
    case ImpactKind::Lost: return "lost";
    case ImpactKind::Stumps: return "stumps";
  }
  return "?";
}

namespace {

using Clock = std::chrono::steady_clock;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

double msSince(Clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

bool debugOn() {
  static const bool on = std::getenv("PE_DEBUG") != nullptr;
  return on;
}

// One way of explaining the track: bounce (after observation kb) or full toss,
// valid for observations 0..end (minus skipped outliers).
struct Hypothesis {
  bool ok = false;
  bool bounce = false;
  int kb = -1;
  int end = -1;
  std::vector<int> skipped;
  bool deviated = false;  // stopped because later points left the flight path (impact)
  FitResult fit;

  bool isSkipped(int i) const { return std::find(skipped.begin(), skipped.end(), i) != skipped.end(); }
  int coverage() const {
    int c = end + 1;
    for (int s : skipped) c -= s <= end ? 1 : 0;
    return c;
  }
};

FitResult fitHypothesis(const Camera& cam, const std::vector<Observation>& obs, const Hypothesis& h,
                        const FitOptions& opt) {
  std::vector<Observation> pre, post;
  for (int i = 0; i <= h.end; ++i) {
    if (h.isSkipped(i)) continue;
    (!h.bounce || i <= h.kb ? pre : post).push_back(obs[i]);
  }
  if (h.bounce && post.empty()) {
    FitResult r;
    r.error = "no observations after the bounce";
    return r;
  }
  return fitTrajectory(cam, pre, post, opt);
}

Trajectory extendedTrajectory(const FitResult& fit, double t_begin, double t_end) {
  ModelSpec spec = fit.spec;
  spec.t_begin = std::min(spec.t_begin, t_begin);
  spec.t_end = std::max(spec.t_end, t_end);
  return buildTrajectory(spec, fit.theta);
}

// What the ball does after hitting the pad is not modelled: every point after
// the end of the free flight costs a fixed amount, a bit more than a well-fitting
// flight point (chi-square ~2 for 2 dof). Points that fit the flight worse than
// this end the flight.
double afterImpactCost(const std::vector<Observation>& obs, int from) {
  const int cnt = static_cast<int>(obs.size()) - from;
  if (cnt <= 0) return 0.0;
  constexpr double kPerPoint = 5.0;
  constexpr double kBreak = 4.0;
  return kPerPoint * cnt + kBreak;
}

double flightCost(const Camera& cam, const FitResult& fit, const std::vector<Observation>& obs,
                  const Hypothesis& h, double sigma) {
  const Trajectory tr = extendedTrajectory(fit, obs.front().t, obs[h.end].t);
  double chi = 0;
  for (int i = 0; i <= h.end; ++i) {
    const FlightPath& path = (!h.bounce || i <= h.kb) ? tr.pre : tr.post;
    const double e2 = (cam.project(path.position(obs[i].t)) - obs[i].uv).squaredNorm();
    chi += std::min(e2 / (sigma * sigma), 25.0);  // capped: single bad detections
  }
  return chi;
}

// Find where the free flight ends: every candidate end index is scored by the
// flight fit up to it plus a generic curve for the points after it.
Hypothesis scan(const Camera& cam, const std::vector<Observation>& obs, Hypothesis h, int m_min,
                const FitOptions& base, double sigma, double* best_total) {
  const int n = static_cast<int>(obs.size());
  FitOptions opt = base;
  opt.max_outliers = 0;
  Hypothesis best = h;
  *best_total = std::numeric_limits<double>::infinity();
  Eigen::VectorXd warm;
  for (int m = std::min(m_min, n - 1); m < n; ++m) {
    Hypothesis c = h;
    c.end = m;
    opt.initial_theta = warm;
    opt.max_iterations = warm.size() ? 30 : 60;
    c.fit = fitHypothesis(cam, obs, c, opt);
    if (!c.fit.ok) continue;
    warm = c.fit.theta;
    const double t_lo = c.bounce ? obs[c.kb].t : 0.0;
    const double t_hi = c.bounce ? obs[c.kb + 1].t : 0.0;
    const double total = flightCost(cam, c.fit, obs, c, sigma) + priorCost(c.fit, base.ball, t_lo, t_hi) +
                         afterImpactCost(obs, m + 1);
    if (total < *best_total) {
      *best_total = total;
      best = c;
      best.ok = true;
      best.deviated = m < n - 1;
    }
  }
  return best;
}

// Indices of the lowest image points (largest v): the bounce is near them.
std::vector<int> bounceCandidates(const std::vector<Observation>& obs) {
  std::vector<int> out;
  const int n = static_cast<int>(obs.size());
  for (int k = 1; k + 1 < n; ++k) {
    const double v = obs[k].uv.y();
    if (v < obs[k - 1].uv.y() || v < obs[k + 1].uv.y()) continue;
    double after_min = v;
    for (int j = k + 1; j < std::min(n, k + 4); ++j) after_min = std::min(after_min, obs[j].uv.y());
    const double before = obs[std::max(0, k - 3)].uv.y();
    if (v - after_min >= 1.5 && v - before >= 1.5) {
      out.push_back(k);
      if (out.size() >= 2) break;
    }
  }
  return out;
}

// Points where the image motion turns sharply (same smoothness measure as the
// path search): the bowler's hand -> ball switch, the bounce, the pad.
std::vector<int> kinkIndices(const std::vector<Observation>& obs) {
  std::vector<int> out;
  for (size_t i = 2; i < obs.size(); ++i) {
    const Observation &z = obs[i - 2], &a = obs[i - 1], &b = obs[i];
    const Vec2 v1 = (a.uv - z.uv) / (a.t - z.t);
    const Vec2 pred = a.uv + v1 * (b.t - a.t);
    const double tol = 1.0 + 0.35 * v1.norm() * (b.t - a.t) + 0.25 * (a.radius_px + b.radius_px);
    if ((b.uv - pred).norm() > 3.0 * tol) out.push_back(static_cast<int>(i));
  }
  return out;
}

struct Derived {
  bool ok = false;
  double t_release = kNaN;
  double release_speed = kNaN;
  double average_speed = kNaN;
  double swing = kNaN;  // world x, metres
  double turn_deg = kNaN;
  Vec3 bounce = Vec3::Constant(kNaN);
  bool reaches_stumps = false;
  Vec3 at_stumps = Vec3::Constant(kNaN);
  Vec3 impact = Vec3::Constant(kNaN);
};

Derived derive(const Trajectory& tr, const AnalysisConfig& cfg, double t_first, double t_impact) {
  Derived d;
  const double L = cfg.pitch.length;
  const double pre_end = tr.has_bounce ? tr.t_bounce : tr.tEnd();
  const auto t_rel = tr.pre.timeAtY(cfg.release_offset_m, tr.tBegin(), pre_end);
  d.t_release = t_rel ? *t_rel : t_first;
  const Vec3 p_rel = tr.position(d.t_release);
  const Vec3 v_rel = tr.velocity(d.t_release);
  if (v_rel.y() <= 0.5) return d;
  d.release_speed = v_rel.norm();

  const auto t_st = tr.timeAtY(L, d.t_release);
  if (t_st) {
    d.reaches_stumps = true;
    d.at_stumps = tr.position(*t_st);
  }
  double t_end = t_st ? *t_st : tr.tEnd();
  if (std::isfinite(t_impact)) {
    t_end = std::min(t_end, t_impact);
    d.impact = tr.position(t_impact);
  }
  if (t_end <= d.t_release) return d;

  double len = 0;
  Vec3 prev = p_rel;
  const int steps = 60;
  for (int i = 1; i <= steps; ++i) {
    const Vec3 p = tr.position(d.t_release + (t_end - d.t_release) * i / steps);
    len += (p - prev).norm();
    prev = p;
  }
  d.average_speed = len / (t_end - d.t_release);

  Vec3 ref;
  if (tr.has_bounce) {
    d.bounce = tr.pre.position(tr.t_bounce);
    ref = d.bounce;
    const Vec3 vin = tr.pre.velocity(tr.t_bounce);
    const Vec3 vout = tr.post.velocity(tr.t_bounce);
    d.turn_deg = (std::atan2(vout.x(), vout.y()) - std::atan2(vin.x(), vin.y())) * 180.0 / kPi;
  } else {
    ref = tr.position(t_end);
  }
  const double x_line = p_rel.x() + v_rel.x() / v_rel.y() * (ref.y() - p_rel.y());
  d.swing = ref.x() - x_line;
  d.ok = true;
  return d;
}

double percentile(std::vector<double> v, double q) {
  v.erase(std::remove_if(v.begin(), v.end(), [](double x) { return !std::isfinite(x); }), v.end());
  if (v.empty()) return kNaN;
  std::sort(v.begin(), v.end());
  const double pos = q * (v.size() - 1);
  const size_t i = static_cast<size_t>(pos);
  const double frac = pos - i;
  return i + 1 < v.size() ? v[i] * (1 - frac) + v[i + 1] * frac : v[i];
}

Estimate makeEstimate(double nominal, const std::vector<double>& samples) {
  Estimate e;
  if (!std::isfinite(nominal)) return e;
  e.value = nominal;
  e.lo = percentile(samples, 0.05);
  e.hi = percentile(samples, 0.95);
  if (!std::isfinite(e.lo) || !std::isfinite(e.hi)) e.lo = e.hi = nominal;
  e.valid = true;
  return e;
}

double fraction(const std::vector<double>& v, const std::function<bool(double)>& pred) {
  if (v.empty()) return kNaN;
  size_t c = 0;
  for (double x : v) c += pred(x) ? 1 : 0;
  return static_cast<double>(c) / v.size();
}

std::string fmt(double v, int prec = 1) {
  std::ostringstream s;
  s.setf(std::ios::fixed);
  s.precision(prec);
  s << v;
  return s.str();
}

}  // namespace

DeliveryResult analyzeTrack(const std::vector<TrackPoint>& track, const AnalysisConfig& cfg) {
  std::vector<Observation> obs;
  obs.reserve(track.size());
  for (const TrackPoint& p : track) obs.push_back({p.t, p.uv, p.radius_px});
  DeliveryResult r = analyzeObservations(obs, cfg);
  if (!track.empty()) {
    r.frame_first = track.front().frame;
    for (const TrackPoint& p : track) {
      if (p.t == r.t_first) r.frame_first = p.frame;  // the start may have been trimmed
    }
    r.frame_last = track.back().frame;
  }
  return r;
}

DeliveryResult analyzeObservations(const std::vector<Observation>& obs_in, const AnalysisConfig& cfg) {
  const auto t_start = Clock::now();
  DeliveryResult res;
  res.observations = obs_in;
  res.labels.assign(obs_in.size(), kOutlier);
  if (obs_in.size() < 6) {
    res.reject_reason = "track too short";
    return res;
  }
  res.t_first = obs_in.front().t;
  res.t_last = obs_in.back().t;
  const Camera& cam = cfg.camera;
  const double L = cfg.pitch.length;
  const double rb = cfg.ball.radius;
  const double sigma = cfg.sigma_px;

  FitOptions fopt;
  fopt.ball = cfg.ball;
  fopt.sigma_px = sigma;
  fopt.pitch_length = L;

  // --- Cheap checks first: most tracks are not balls (edges of people, noise).
  {
    double disp = 0;
    for (const Observation& o : obs_in) disp = std::max(disp, (o.uv - obs_in.front().uv).norm());
    if (obs_in.back().t - obs_in.front().t < 0.12 || disp < std::max(25.0, 0.035 * cam.height)) {
      res.reject_reason = "track too short or not moving";
      return res;
    }
  }
  // A path can start on the bowler's hand before the release. Candidate starts
  // are the beginning and every sharp turn; the stretch from a start to the
  // next turn (or the first lowest point, the bounce) must be a free flight
  // moving away from the camera at a plausible speed.
  int start = -1;
  {
    const int n_in = static_cast<int>(obs_in.size());
    const std::vector<int> kinks = kinkIndices(obs_in);
    std::vector<int> starts;
    for (int k : kinks) {
      starts.push_back(k - 1);
      starts.push_back(k);
    }
    // The hand can also blend smoothly into the ball: also try regular steps.
    for (int i = 0; i < std::min(n_in - 6, 24); i += 2) starts.push_back(i);
    std::sort(starts.begin(), starts.end());
    starts.erase(std::unique(starts.begin(), starts.end()), starts.end());
    FitOptions o = fopt;
    o.max_outliers = 1;
    o.max_iterations = 25;
    for (int s0 : starts) {
      if (s0 < 0 || s0 + 6 > n_in || (start >= 0)) continue;
      int end = std::min(n_in, s0 + 14);
      for (int k : kinks) {
        if (k > s0 + 1) {
          end = std::min(end, k);
          break;
        }
      }
      const std::vector<Observation> rest(obs_in.begin() + s0, obs_in.end());
      const auto lows = bounceCandidates(rest);
      if (!lows.empty() && lows.front() >= 4) end = std::min(end, s0 + lows.front() + 1);
      if (end - s0 < 7) continue;  // fewer points: an 8-parameter flight fits anything
      const std::vector<Observation> head(obs_in.begin() + s0, obs_in.begin() + end);
      const FitResult f = fitTrajectory(cam, head, {}, o);
      if (!f.ok || f.rms_px > 2.0 * sigma + 1.0) continue;
      const Vec3 v(f.theta[theta::VX], f.theta[theta::VY], f.theta[theta::VZ]);
      const Trajectory tr = buildTrajectory(f.spec, f.theta);
      const Vec3 p0 = tr.position(head.front().t);
      if (v.y() < 2.0 || v.norm() < 4.0 || v.norm() > 50.0) continue;
      if (p0.y() < -0.5 || p0.y() > 0.75 * L || p0.z() < 0.0 || p0.z() > 3.0) continue;
      // Blob sizes must match a ball at the fitted distances.
      std::vector<double> ratios;
      for (const Observation& ob : head) {
        if (ob.radius_px > 0) ratios.push_back(ob.radius_px / cam.apparentRadius(tr.position(ob.t), rb));
      }
      std::nth_element(ratios.begin(), ratios.begin() + ratios.size() / 2, ratios.end());
      if (ratios.empty() || ratios[ratios.size() / 2] < 0.45 || ratios[ratios.size() / 2] > 2.2) continue;
      if (debugOn()) {
        std::fprintf(stderr, "start %d (of %d): head %d..%d rms %.2f v=(%.1f %.1f %.1f) p0=(%.2f %.2f %.2f)\n",
                     s0, n_in, s0, end - 1, f.rms_px, v.x(), v.y(), v.z(), p0.x(), p0.y(), p0.z());
      }
      start = s0;
    }
  }
  if (start < 0) {
    res.reject_reason = "start of track is not a ball flight";
    return res;
  }
  const std::vector<Observation> obs(obs_in.begin() + start, obs_in.end());
  const int n = static_cast<int>(obs.size());
  if (n < 6) {
    res.reject_reason = "track too short";
    return res;
  }
  res.observations = obs;
  res.labels.assign(obs.size(), kOutlier);
  res.t_first = obs.front().t;

  // --- Segmentation: full toss vs bounce, and where the free flight ends (impact).
  const auto t_seg = Clock::now();
  std::vector<Hypothesis> hyps;
  std::vector<double> totals;
  const double log_n = std::log(static_cast<double>(n));
  double total = 0;
  Hypothesis ft;
  ft = scan(cam, obs, ft, std::min(5, n - 1), fopt, sigma, &total);
  if (ft.ok) {
    hyps.push_back(ft);
    totals.push_back(total + 8 * log_n);
  }

  std::vector<int> kbs;
  for (int k : bounceCandidates(obs)) {
    for (int kb : {k - 1, k}) kbs.push_back(kb);
  }
  if (ft.ok) {
    // Where the free flight reaches the ground (or leaves its path near the
    // ground) the ball has bounced.
    const Trajectory tr = extendedTrajectory(ft.fit, obs.front().t, obs.back().t + 0.5);
    if (ft.deviated && tr.position(0.5 * (obs[ft.end].t + obs[ft.end + 1].t)).z() < rb + 0.15) {
      kbs.push_back(ft.end);
      kbs.push_back(ft.end - 1);
    }
    for (int i = 0; i + 1 < n; ++i) {
      if (tr.position(obs[i].t).z() > rb && tr.position(obs[i + 1].t).z() <= rb) {
        kbs.push_back(i);
        kbs.push_back(i - 1);
        kbs.push_back(i + 1);
        break;
      }
    }
  }
  std::sort(kbs.begin(), kbs.end());
  kbs.erase(std::unique(kbs.begin(), kbs.end()), kbs.end());

  // Pick the best bounce index on a common window (same points for every
  // candidate, so the costs are comparable), then scan for the impact.
  Hypothesis best_bounce;
  double best_cost = std::numeric_limits<double>::infinity();
  int window_end = 0;
  for (int kb : kbs) window_end = std::max(window_end, kb + 3);
  window_end = std::min(window_end, n - 1);
  for (int kb : kbs) {
    if (kb < 3 || kb + 1 > window_end) continue;
    Hypothesis h;
    h.bounce = true;
    h.kb = kb;
    h.end = window_end;
    FitOptions o = fopt;
    o.max_outliers = 0;
    h.fit = fitHypothesis(cam, obs, h, o);
    if (!h.fit.ok) continue;
    const double cost = flightCost(cam, h.fit, obs, h, sigma) +
                        priorCost(h.fit, fopt.ball, obs[kb].t, obs[kb + 1].t);
    if (cost < best_cost) {
      best_cost = cost;
      best_bounce = h;
    }
  }
  if (best_bounce.kb >= 0) {
    Hypothesis h = scan(cam, obs, best_bounce, best_bounce.kb + 1, fopt, sigma, &total);
    if (h.ok) {
      hyps.push_back(h);
      totals.push_back(total + 10 * log_n);  // priors tie down part of the bounce model
    }
  }
  if (hyps.empty()) {
    res.reject_reason = "no consistent flight path";
    return res;
  }
  const double rms_ok = std::max(2.0, 2.5 * sigma);
  const Hypothesis* chosen = &hyps[std::min_element(totals.begin(), totals.end()) - totals.begin()];
  if (debugOn()) {
    for (const Hypothesis& h : hyps) {
      std::fprintf(stderr, "hyp bounce=%d kb=%d end=%d/%d dev=%d rms=%.2f total=%.1f%s\n", h.bounce, h.kb,
                   h.end, n - 1, h.deviated, h.fit.rms_px, totals[&h - hyps.data()],
                   &h == chosen ? " *" : "");
    }
  }
  Hypothesis hyp = *chosen;
  res.timings_ms.push_back({"segment", msSince(t_seg)});

  // --- Final fit with outlier rejection.
  const auto t_fit = Clock::now();
  {
    FitOptions o = fopt;
    o.initial_theta = hyp.fit.theta;
    FitResult f = fitHypothesis(cam, obs, hyp, o);
    if (f.ok) hyp.fit = std::move(f);
  }
  const FitResult& fit = hyp.fit;
  res.fit = fit;
  res.timings_ms.push_back({"fit", msSince(t_fit)});
  if (!fit.ok) {
    res.reject_reason = "trajectory fit failed: " + fit.error;
    return res;
  }
  res.has_bounce = hyp.bounce;
  res.rms_px = fit.rms_px;
  res.n_pre = fit.n_pre;
  res.n_post = fit.n_post;
  res.n_outliers = fit.n_outliers + static_cast<int>(hyp.skipped.size());

  // --- Impact.
  int first_after = -1;
  for (int i = hyp.end + 1; i < n; ++i) {
    if (!hyp.isSkipped(i)) {
      first_after = i;
      break;
    }
  }
  const double t_extend_end = (hyp.bounce ? fit.theta[theta::TB] : obs[hyp.end].t) + 1.5;
  const Trajectory traj = extendedTrajectory(fit, obs.front().t - 1.0, t_extend_end);
  double t_imp = kNaN;
  ImpactKind kind = ImpactKind::None;
  if (hyp.deviated && first_after >= 0) {
    t_imp = 0.5 * (obs[hyp.end].t + obs[first_after].t);
    kind = ImpactKind::Pad;
  } else {
    const double y_last = traj.position(obs[hyp.end].t).y();
    if (y_last < L - 0.4) {
      const double dt = (obs[hyp.end].t - obs.front().t) / std::max(1, hyp.end);
      t_imp = obs[hyp.end].t + 0.5 * dt;
      kind = ImpactKind::Lost;
    }
  }
  if (kind != ImpactKind::None) {
    const Vec3 p = traj.position(t_imp);
    if (p.y() > L + 0.3) {
      kind = ImpactKind::None;
    } else if (p.y() >= L - 0.10) {
      kind = ImpactKind::Stumps;
    }
    if (kind == ImpactKind::None) t_imp = kNaN;
  }
  res.impact = kind;
  res.t_impact = t_imp;

  for (int i = 0; i < n; ++i) {
    if (hyp.isSkipped(i)) continue;
    if (i <= hyp.end) {
      res.labels[i] = (!hyp.bounce || i <= hyp.kb) ? kPreBounce : kPostBounce;
    } else if (kind == ImpactKind::Pad || kind == ImpactKind::Stumps) {
      res.labels[i] = kAfterImpact;
      ++res.n_after_impact;
    }
  }
  for (double t : fit.outlier_times) {
    for (int i = 0; i < n; ++i) {
      if (obs[i].t == t) res.labels[i] = kOutlier;
    }
  }

  // --- Metrics (nominal) and plausibility.
  const Derived nom = derive(traj, cfg, obs.front().t, t_imp);
  if (!nom.ok) {
    res.reject_reason = "ball is not moving towards the batter";
    return res;
  }
  const double y_first = traj.position(obs.front().t).y();
  const double y_last = traj.position(obs[hyp.end].t).y();
  if (nom.release_speed < 4.0 || nom.release_speed > 50.0) {
    res.reject_reason = "implausible speed " + fmt(nom.release_speed * 3.6) + " km/h";
    return res;
  }
  // The ball must be picked up in the bowler's half; a flight that only starts
  // near the batter cannot be told apart from a rebound off the pad.
  if (y_first > 0.55 * L || y_last < 0.35 * L || y_last - y_first < 0.25 * L) {
    res.reject_reason = "track does not cover enough of the pitch";
    return res;
  }
  if (hyp.bounce && (nom.bounce.y() < 0.3 || nom.bounce.y() > L + 0.5)) {
    res.reject_reason = "bounce point outside the pitch";
    return res;
  }
  if (fit.rms_px > 2.0 * rms_ok) {
    res.reject_reason = "track does not follow a ball flight (rms " + fmt(fit.rms_px) + " px)";
    return res;
  }
  {
    // Blobs must have the size a ball would have at the fitted distance
    // (fragments of people are often much smaller or larger).
    std::vector<double> ratios;
    for (int i = 0; i <= hyp.end; ++i) {
      if (res.labels[i] == kOutlier || obs[i].radius_px <= 0) continue;
      const double model = cam.apparentRadius(traj.position(obs[i].t), rb);
      if (model >= 3.0) ratios.push_back(obs[i].radius_px / model);
    }
    if (ratios.size() >= 3) {
      std::nth_element(ratios.begin(), ratios.begin() + ratios.size() / 2, ratios.end());
      const double med = ratios[ratios.size() / 2];
      if (med < 0.45 || med > 2.2) {
        res.reject_reason = "blob size does not match a ball at that distance";
        return res;
      }
    }
  }

  // --- Monte-Carlo uncertainty.
  const auto t_mc = Clock::now();
  const Eigen::Index np = fit.theta.size();
  Eigen::MatrixXd S;
  {
    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(fit.covariance);
    const Eigen::VectorXd ev = es.eigenvalues().cwiseMax(0.0).cwiseSqrt();
    S = es.eigenvectors() * ev.asDiagonal();
  }
  std::mt19937 rng(cfg.seed);
  std::normal_distribution<double> nd(0.0, 1.0);
  const double so = offSign(cfg.batter);
  std::vector<double> s_rel, s_avg, s_swing, s_turn, s_bx, s_len, s_sx, s_sz, s_ix, s_iz, s_hit;
  ModelSpec spec_ext = fit.spec;
  spec_ext.t_begin = std::min(spec_ext.t_begin, obs.front().t - 1.0);
  spec_ext.t_end = std::max(spec_ext.t_end, t_extend_end);
  const double edge = 0.5 * cfg.pitch.stump_width + rb;
  for (int s = 0; s < cfg.mc_samples; ++s) {
    Eigen::VectorXd z(np);
    for (Eigen::Index i = 0; i < np; ++i) z[i] = nd(rng);
    const Eigen::VectorXd th = fit.theta + S * z;
    const Trajectory tr = buildTrajectory(spec_ext, th);
    const Derived d = derive(tr, cfg, obs.front().t, t_imp);
    if (!d.ok) continue;
    const double cx = cfg.calib_sigma_x_m * nd(rng);
    const double cz = cfg.calib_sigma_z_m * nd(rng);
    s_rel.push_back(d.release_speed * 3.6);
    s_avg.push_back(d.average_speed * 3.6);
    s_swing.push_back(so * d.swing * 100.0);
    if (hyp.bounce) {
      s_turn.push_back(so * d.turn_deg);
      s_bx.push_back(so * (d.bounce.x() + cx) * 100.0);
      s_len.push_back(L - d.bounce.y());
    }
    if (d.reaches_stumps) {
      const double sx = d.at_stumps.x() + cx, sz = d.at_stumps.z() + cz;
      s_sx.push_back(so * sx * 100.0);
      s_sz.push_back(sz * 100.0);
      s_hit.push_back(std::abs(sx) <= edge && sz <= cfg.pitch.stump_height + rb ? 1.0 : 0.0);
    } else {
      s_hit.push_back(0.0);
    }
    if (std::isfinite(t_imp)) {
      s_ix.push_back(so * (d.impact.x() + cx) * 100.0);
      s_iz.push_back(d.impact.z() * 100.0);
    }
  }
  res.timings_ms.push_back({"monte_carlo", msSince(t_mc)});

  res.release_speed_kmh = makeEstimate(nom.release_speed * 3.6, s_rel);
  res.average_speed_kmh = makeEstimate(nom.average_speed * 3.6, s_avg);
  res.swing_cm = makeEstimate(so * nom.swing * 100.0, s_swing);
  const double sw = res.swing_cm.value;
  res.swing_type = sw > 2.0 ? "out-swing" : (sw < -2.0 ? "in-swing" : "straight");
  if (hyp.bounce) {
    res.t_bounce = fit.theta[theta::TB];
    res.turn_deg = makeEstimate(so * nom.turn_deg, s_turn);
    const double tu = res.turn_deg.value;
    res.turn_type = tu > 0.5 ? "towards off" : (tu < -0.5 ? "towards leg" : "straight");
    res.bounce_x_off_cm = makeEstimate(so * nom.bounce.x() * 100.0, s_bx);
    res.length_m = makeEstimate(L - nom.bounce.y(), s_len);
    res.length_zone = lengthZone(res.length_m.value, L);
    res.line_zone = lineZone(res.bounce_x_off_cm.value / 100.0, cfg.pitch, rb);
  } else {
    res.length_zone = "full toss";
  }
  res.reaches_stumps = nom.reaches_stumps;
  if (nom.reaches_stumps) {
    res.stumps_x_off_cm = makeEstimate(so * nom.at_stumps.x() * 100.0, s_sx);
    res.stumps_height_cm = makeEstimate(nom.at_stumps.z() * 100.0, s_sz);
  }
  if (std::isfinite(t_imp)) {
    res.impact_point = nom.impact;
    res.impact_x_off_cm = makeEstimate(so * nom.impact.x() * 100.0, s_ix);
    res.impact_height_cm = makeEstimate(nom.impact.z() * 100.0, s_iz);
    res.projection_m = L - nom.impact.y();
  }

  // --- LBW.
  LbwInput li;
  li.full_toss = !hyp.bounce;
  li.no_shot_offered = cfg.no_shot_offered;
  li.sure = cfg.decision_probability;
  const double edge_cm = edge * 100.0;
  if (hyp.bounce) {
    li.p_pitch_outside_leg = fraction(s_bx, [&](double x) { return x < -edge_cm; });
    li.p_pitch_outside_off = fraction(s_bx, [&](double x) { return x > edge_cm; });
  }
  li.p_impact_outside_leg = fraction(s_ix, [&](double x) { return x < -edge_cm; });
  li.p_impact_outside_off = fraction(s_ix, [&](double x) { return x > edge_cm; });
  li.p_hit = fraction(s_hit, [](double h) { return h > 0.5; });
  for (double* p : {&li.p_pitch_outside_leg, &li.p_pitch_outside_off, &li.p_impact_outside_leg,
                    &li.p_impact_outside_off, &li.p_hit}) {
    if (!std::isfinite(*p)) *p = 0.0;
  }
  res.lbw_input = li;
  res.lbw = decideLbw(li);
  res.lbw_applicable = kind == ImpactKind::Pad || kind == ImpactKind::Lost;

  // --- Notes on reliability.
  if (res.lbw_applicable && hyp.bounce && res.n_post < 3) {
    res.warnings.push_back("only " + std::to_string(res.n_post) +
                           " tracked frame(s) between bounce and impact: turn and projection "
                           "are less certain");
  }
  if (res.lbw_applicable && res.projection_m > 2.5) {
    res.warnings.push_back("long projection (" + fmt(res.projection_m) +
                           " m from impact to stumps): less reliable");
  }
  if (kind == ImpactKind::Lost) {
    res.warnings.push_back("ball lost from view before the stumps: impact assumed at its last "
                           "seen position (check the replay)");
  }
  if (kind == ImpactKind::Stumps) {
    res.warnings.push_back("ball deflected at the stumps (bowled?)");
  }
  if (y_first - cfg.release_offset_m > 0.35 * L) {
    res.warnings.push_back("ball first seen late: release speed is extrapolated");
  }
  if (fit.rms_px > rms_ok) {
    res.warnings.push_back("noisy track (" + fmt(fit.rms_px) + " px rms)");
  }

  // --- Paths for drawing.
  const double t_path_end =
      std::isfinite(t_imp) ? t_imp : (nom.reaches_stumps ? *traj.timeAtY(L, nom.t_release) : obs[hyp.end].t);
  for (int i = 0; i <= 60; ++i) {
    const double t = nom.t_release + (t_path_end - nom.t_release) * i / 60.0;
    res.path.push_back({t, traj.position(t)});
  }
  if (res.lbw_applicable && nom.reaches_stumps) {
    const double t_st = *traj.timeAtY(L, nom.t_release);
    for (int i = 0; i <= 20; ++i) {
      const double t = t_imp + (t_st - t_imp) * i / 20.0;
      res.projection.push_back({t, traj.position(t)});
    }
  }

  res.valid = true;
  res.timings_ms.push_back({"analysis_total", msSince(t_start)});
  return res;
}

}  // namespace pe
