#include "pocketeye/fit.hpp"

#include <Eigen/Dense>
#include <algorithm>
#include <cmath>
#include <limits>

#include "pocketeye/lm.hpp"

namespace pe {

using namespace theta;

Trajectory buildTrajectory(const ModelSpec& spec, const Eigen::VectorXd& th, double max_step) {
  Trajectory tr;
  tr.has_bounce = spec.has_bounce;
  if (spec.has_bounce) {
    const Vec3 b(th[XB], th[YB], spec.ball_radius);
    const double tb = th[TB];
    tr.t_bounce = tb;
    const FlightParams pre_fp{std::exp(th[LOGK_B]), th[ALAT]};
    const FlightParams post_fp{std::exp(th[LOGK_B]), 0.0};
    tr.pre = FlightPath(b, Vec3(th[VIX], th[VIY], th[VIZ]), tb, spec.t_begin, tb, pre_fp, max_step);
    tr.post = FlightPath(b, Vec3(th[VOX], th[VOY], th[VOZ]), tb, tb, spec.t_end, post_fp, max_step);
  } else {
    const FlightParams fp{std::exp(th[LOGK_F]), th[ALAT_F]};
    tr.pre = FlightPath(Vec3(th[X], th[Y], th[Z]), Vec3(th[VX], th[VY], th[VZ]), spec.t_ref,
                        spec.t_begin, spec.t_end, fp, max_step);
  }
  return tr;
}

namespace {

struct Problem {
  const Camera* cam;
  const FitOptions* opt;
  std::vector<Observation> pre;
  std::vector<Observation> post;
  ModelSpec spec;
};

void observationResiduals(const Problem& pb, const FlightPath& path,
                          const std::vector<Observation>& obs, Eigen::VectorXd& r,
                          Eigen::Index& k) {
  const double rb = pb.spec.ball_radius;
  for (const Observation& o : obs) {
    const Vec3 p = path.position(o.t);
    const double z = pb.cam->depth(p);
    if (z < 0.3) {
      r[k++] = 1e3;
      r[k++] = 1e3;
      r[k++] = o.radius_px > 0 ? 1e3 : 0.0;
      continue;
    }
    const Vec2 uv = pb.cam->project(p);
    r[k++] = (uv.x() - o.uv.x()) / pb.opt->sigma_px;
    r[k++] = (uv.y() - o.uv.y()) / pb.opt->sigma_px;
    if (o.radius_px > 0) {
      const double rho = pb.cam->apparentRadius(p, rb);
      r[k++] = (rho - o.radius_px) /
               (pb.opt->radius_sigma_px + pb.opt->radius_sigma_rel * o.radius_px);
    } else {
      r[k++] = 0.0;
    }
  }
}

// Soft physical priors (see BallSpec): swing size, drag, and how much vertical
// and along-pitch speed the bounce keeps. Kept tight enough that a fit cannot
// absorb a rebound off the pad by pretending the ball slowed down at the bounce.
void priorResiduals(bool bounce, const Eigen::VectorXd& th, const BallSpec& ball, double t_lo,
                    double t_hi, Eigen::VectorXd& r, Eigen::Index& k) {
  if (bounce) {
    const double viz = th[VIZ], viy = th[VIY];
    r[k++] = th[ALAT] / 6.0;
    r[k++] = (th[LOGK_B] - std::log(ball.drag_k)) / 0.6;
    r[k++] = (th[VOZ] - ball.restitution * (-viz)) / (0.2 * std::abs(viz) + 0.3);
    r[k++] = (th[VOY] - ball.friction * viy) / (0.1 * std::abs(viy) + 0.3);
    const double turn = std::atan2(th[VOX], th[VOY]) - std::atan2(th[VIX], th[VIY]);
    r[k++] = turn / 0.25;
    // Keep the bounce time between the last pre-bounce and first post-bounce frame.
    r[k++] = th[TB] < t_lo ? (t_lo - th[TB]) / 0.002 : 0.0;
    r[k++] = th[TB] > t_hi ? (th[TB] - t_hi) / 0.002 : 0.0;
  } else {
    r[k++] = th[ALAT_F] / 6.0;
    r[k++] = (th[LOGK_F] - std::log(ball.drag_k)) / 0.6;
  }
}

int residualCount(const Problem& pb) {
  const int n_obs = static_cast<int>(pb.pre.size() + pb.post.size());
  return 3 * n_obs + (pb.spec.has_bounce ? 7 : 2);
}

void residuals(const Problem& pb, const Eigen::VectorXd& th, Eigen::VectorXd& r) {
  r.setZero(residualCount(pb));
  Eigen::Index k = 0;
  const Trajectory tr = buildTrajectory(pb.spec, th);
  const BallSpec& ball = pb.opt->ball;
  observationResiduals(pb, tr.pre, pb.pre, r, k);
  if (pb.spec.has_bounce) observationResiduals(pb, tr.post, pb.post, r, k);
  const double t_lo = pb.spec.has_bounce ? pb.pre.back().t : 0.0;
  const double t_hi = pb.spec.has_bounce ? pb.post.front().t : 0.0;
  priorResiduals(pb.spec.has_bounce, th, ball, t_lo, t_hi, r, k);
}

// Least squares helpers for the grid initialisation.
double fitThroughOrigin(const std::vector<double>& tau, const std::vector<double>& y, double* slope) {
  double stt = 0, sty = 0;
  for (size_t i = 0; i < tau.size(); ++i) {
    stt += tau[i] * tau[i];
    sty += tau[i] * y[i];
  }
  *slope = stt > 0 ? sty / stt : 0.0;
  double sse = 0;
  for (size_t i = 0; i < tau.size(); ++i) sse += std::pow(y[i] - *slope * tau[i], 2);
  return sse;
}

// Small polynomial least squares (up to 3 coefficients) via normal equations.
double fitPoly(const std::vector<double>& tau, const std::vector<double>& y, int degree,
               bool with_constant, Eigen::VectorXd* coef) {
  const int cols = degree + (with_constant ? 1 : 0);
  Eigen::Matrix3d AtA = Eigen::Matrix3d::Zero();
  Eigen::Vector3d Atb = Eigen::Vector3d::Zero();
  double yy = 0;
  for (size_t i = 0; i < tau.size(); ++i) {
    Eigen::Vector3d r = Eigen::Vector3d::Zero();
    int c = 0;
    if (with_constant) r[c++] = 1.0;
    double p = 1.0;
    for (int d = 1; d <= degree; ++d) r[c++] = (p *= tau[i]);
    AtA += r * r.transpose();
    Atb += r * y[i];
    yy += y[i] * y[i];
  }
  const Eigen::MatrixXd M = AtA.topLeftCorner(cols, cols);
  const Eigen::VectorXd v = Atb.head(cols);
  *coef = M.ldlt().solve(v);
  return std::max(0.0, yy - 2.0 * coef->dot(v) + coef->dot(M * *coef));
}

struct SegmentInit {
  double cost = std::numeric_limits<double>::infinity();
  Vec3 v = Vec3::Zero();
  Vec3 p = Vec3::Zero();
  double ax = 0.0;
};

double radiusCost(const Camera& cam, const Vec3& p, double rb, const Observation& o) {
  if (o.radius_px <= 0) return 0.0;
  const double rho = cam.apparentRadius(p, rb);
  const double s = 0.8 + 0.25 * o.radius_px;
  return 0.3 * std::pow((rho - o.radius_px) / s, 2);
}

// Grid search over the along-pitch speed for a segment anchored at the bounce point.
SegmentInit initAnchored(const Camera& cam, const std::vector<Observation>& obs, const Vec3& b,
                         double tb, double vy_lo, double vy_hi, bool lateral_accel, double rb) {
  SegmentInit best;
  const Vec3 c = cam.center();
  std::vector<Vec3> rays;
  for (const Observation& o : obs) rays.push_back(cam.rayDirection(o.uv));
  for (double vy = vy_lo; vy <= vy_hi; vy += 0.25) {
    std::vector<double> tau, xs, zs;
    double cost = 0;
    for (size_t i = 0; i < obs.size(); ++i) {
      const Observation& o = obs[i];
      const double tt = o.t - tb;
      const auto hit = intersectPlaneY(c, rays[i], b.y() + vy * tt);
      if (!hit) {
        cost += 1e4;
        continue;
      }
      tau.push_back(tt);
      xs.push_back(hit->x() - b.x());
      zs.push_back(hit->z() - b.z() + 0.5 * kGravity * tt * tt);
      cost += radiusCost(cam, *hit, rb, o);
    }
    if (tau.size() < 2) continue;
    double vz = 0;
    cost += fitThroughOrigin(tau, zs, &vz) / 0.01;
    Eigen::VectorXd cx;
    cost += fitPoly(tau, xs, (lateral_accel && tau.size() >= 3) ? 2 : 1, false, &cx) / 0.01;
    if (cost < best.cost) {
      best.cost = cost;
      best.v = Vec3(cx[0], vy, vz);
      best.ax = cx.size() > 1 ? 2.0 * cx[1] : 0.0;
    }
  }
  return best;
}

// Grid over (y at t_ref, vy) for a full toss.
SegmentInit initFree(const Camera& cam, const std::vector<Observation>& obs, double t_ref,
                     double y_max, double rb) {
  SegmentInit best;
  const Vec3 c = cam.center();
  std::vector<Vec3> rays;
  for (const Observation& o : obs) rays.push_back(cam.rayDirection(o.uv));
  std::vector<double> tau, xs, zs;
  auto evaluate = [&](double y0, double vy) {
    tau.clear();
    xs.clear();
    zs.clear();
    double cost = 0;
    for (size_t i = 0; i < obs.size(); ++i) {
      const double tt = obs[i].t - t_ref;
      const auto hit = intersectPlaneY(c, rays[i], y0 + vy * tt);
      if (!hit) {
        cost += 1e4;
        continue;
      }
      tau.push_back(tt);
      xs.push_back(hit->x());
      zs.push_back(hit->z() + 0.5 * kGravity * tt * tt);
      cost += radiusCost(cam, *hit, rb, obs[i]);
    }
    if (tau.size() < 3) return;
    Eigen::VectorXd cz, cx;
    cost += fitPoly(tau, zs, 1, true, &cz) / 0.01;
    cost += fitPoly(tau, xs, 2, true, &cx) / 0.01;
    if (cost < best.cost) {
      best.cost = cost;
      best.p = Vec3(cx[0], y0, cz[0]);
      best.v = Vec3(cx[1], vy, cz[1]);
      best.ax = 2.0 * cx[2];
    }
  };
  // Coarse grid, then refine around the best cell.
  for (double y0 = 0.5; y0 <= y_max; y0 += 0.5) {
    for (double vy = 2.0; vy <= 50.0; vy += 1.5) evaluate(y0, vy);
  }
  if (!std::isfinite(best.cost)) return best;
  const double y_c = best.p.y(), v_c = best.v.y();
  for (double y0 = y_c - 0.5; y0 <= y_c + 0.5; y0 += 0.1) {
    for (double vy = std::max(1.0, v_c - 1.5); vy <= v_c + 1.5; vy += 0.25) evaluate(y0, vy);
  }
  return best;
}

Eigen::VectorXd initialTheta(const Problem& pb, bool* ok) {
  const Camera& cam = *pb.cam;
  const BallSpec& ball = pb.opt->ball;
  const double rb = pb.spec.ball_radius;
  *ok = false;
  if (pb.spec.has_bounce) {
    Eigen::VectorXd th(kBounceSize);
    const double tb0 = 0.5 * (pb.pre.back().t + pb.post.front().t);
    const Vec2 uvb = 0.5 * (pb.pre.back().uv + pb.post.front().uv);
    const auto b = intersectPlaneZ(cam.center(), cam.rayDirection(uvb), rb);
    if (!b) return th;
    const SegmentInit in = initAnchored(cam, pb.pre, *b, tb0, 2.0, 50.0, true, rb);
    if (!std::isfinite(in.cost)) return th;
    Vec3 vout(in.v.x(), ball.friction * in.v.y(), ball.restitution * std::abs(in.v.z()));
    if (pb.post.size() >= 2) {
      const SegmentInit out = initAnchored(cam, pb.post, *b, tb0, 0.3 * in.v.y(),
                                           1.05 * in.v.y(), false, rb);
      if (std::isfinite(out.cost)) vout = out.v;
    }
    th << b->x(), b->y(), tb0, in.v.x(), in.v.y(), in.v.z(), std::clamp(in.ax, -10.0, 10.0),
        vout.x(), vout.y(), vout.z(), std::log(ball.drag_k);
    *ok = true;
    return th;
  }
  Eigen::VectorXd th(kFullTossSize);
  const SegmentInit in = initFree(cam, pb.pre, pb.spec.t_ref, pb.opt->pitch_length + 1.5, rb);
  if (!std::isfinite(in.cost)) return th;
  th << in.p.x(), in.p.y(), in.p.z(), in.v.x(), in.v.y(), in.v.z(),
      std::clamp(in.ax, -10.0, 10.0), std::log(ball.drag_k);
  *ok = true;
  return th;
}

std::vector<double> pixelErrors(const Problem& pb, const Eigen::VectorXd& th) {
  const Trajectory tr = buildTrajectory(pb.spec, th);
  std::vector<double> e;
  for (const Observation& o : pb.pre) e.push_back((pb.cam->project(tr.pre.position(o.t)) - o.uv).norm());
  for (const Observation& o : pb.post) e.push_back((pb.cam->project(tr.post.position(o.t)) - o.uv).norm());
  return e;
}

}  // namespace

double priorCost(const FitResult& fit, const BallSpec& ball, double t_lo, double t_hi) {
  Eigen::VectorXd r(7);
  r.setZero();
  Eigen::Index k = 0;
  priorResiduals(fit.spec.has_bounce, fit.theta, ball, t_lo, t_hi, r, k);
  return r.head(k).squaredNorm();
}

FitResult fitTrajectory(const Camera& cam, const std::vector<Observation>& pre,
                        const std::vector<Observation>& post, const FitOptions& opt) {
  FitResult res;
  const bool bounce = !post.empty();
  if (pre.size() < (bounce ? 3u : 4u)) {
    res.error = "too few observations";
    return res;
  }
  Problem pb;
  pb.cam = &cam;
  pb.opt = &opt;
  pb.pre = pre;
  pb.post = post;
  pb.spec.has_bounce = bounce;
  pb.spec.ball_radius = opt.ball.radius;
  pb.spec.t_begin = pre.front().t - 0.05;
  pb.spec.t_end = (bounce ? post.back().t : pre.back().t) + 0.05;
  pb.spec.t_ref = pre.back().t;

  bool ok = false;
  Eigen::VectorXd th;
  const Eigen::Index expected = bounce ? static_cast<Eigen::Index>(kBounceSize)
                                        : static_cast<Eigen::Index>(kFullTossSize);
  if (opt.initial_theta.size() == expected && opt.initial_theta.allFinite()) {
    th = opt.initial_theta;
    if (bounce) th[TB] = std::clamp(th[TB], pb.pre.back().t, pb.post.front().t);
    ok = true;
  } else {
    th = initialTheta(pb, &ok);
  }
  if (!ok) {
    res.error = "initialisation failed";
    return res;
  }

  LMResult lm;
  for (int round = 0; round <= opt.max_outliers; ++round) {
    const ResidualFn fn = [&pb](const Eigen::VectorXd& x, Eigen::VectorXd& r) { residuals(pb, x, r); };
    LMOptions lo;
    lo.max_iterations = opt.max_iterations;
    lm = solveLM(fn, th, lo);
    th = lm.x;
    const std::vector<double> err = pixelErrors(pb, th);
    double sse = 0;
    for (double e : err) sse += e * e;
    const double rms = std::sqrt(sse / err.size());
    const auto worst = std::max_element(err.begin(), err.end());
    const double limit = std::max(4.0 * opt.sigma_px, 3.0 * rms);
    if (round == opt.max_outliers || *worst <= limit) break;
    const size_t idx = static_cast<size_t>(worst - err.begin());
    if (idx < pb.pre.size()) {
      if (pb.pre.size() <= (bounce ? 3u : 4u)) break;
      res.outlier_times.push_back(pb.pre[idx].t);
      pb.pre.erase(pb.pre.begin() + idx);
    } else {
      if (pb.post.size() <= 1) break;
      res.outlier_times.push_back(pb.post[idx - pb.pre.size()].t);
      pb.post.erase(pb.post.begin() + (idx - pb.pre.size()));
    }
    ++res.n_outliers;
    pb.spec.t_ref = pb.pre.back().t;
  }

  const std::vector<double> err = pixelErrors(pb, th);
  double sse = 0;
  for (double e : err) sse += e * e;
  res.rms_px = std::sqrt(sse / err.size());
  res.theta = th;
  res.cost = lm.cost;
  res.covariance = covarianceFromJacobian(lm.jacobian, lm.cost);
  res.spec = pb.spec;
  res.n_pre = static_cast<int>(pb.pre.size());
  res.n_post = static_cast<int>(pb.post.size());
  res.ok = th.allFinite();
  if (!res.ok) res.error = "fit diverged";
  return res;
}

}  // namespace pe
