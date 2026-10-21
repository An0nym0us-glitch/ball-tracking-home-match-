#include "pocketeye/calibration.hpp"

#include <Eigen/Dense>
#include <algorithm>
#include <cmath>
#include <opencv2/calib3d.hpp>
#include <set>
#include <sstream>

#include "pocketeye/lm.hpp"

namespace pe {

std::optional<Vec3> tapWorldPoint(const std::string& name, const Pitch& p) {
  const double xs = p.outerStumpX();
  const double xc = 0.5 * p.width;
  struct Entry {
    const char* name;
    double x, y_frac, z;  // y_frac: 0 = bowler end, 1 = batter end
    bool stump_top;
  };
  static const Entry kEntries[] = {
      {"bowler_left_base", -1, 0, 0, false},   {"bowler_left_top", -1, 0, 1, true},
      {"bowler_right_base", 1, 0, 0, false},   {"bowler_right_top", 1, 0, 1, true},
      {"batter_left_base", -1, 1, 0, false},   {"batter_left_top", -1, 1, 1, true},
      {"batter_right_base", 1, 1, 0, false},   {"batter_right_top", 1, 1, 1, true},
  };
  for (const auto& e : kEntries) {
    if (name == e.name) {
      return Vec3(e.x * xs, e.y_frac * p.length, e.stump_top ? p.stump_height : 0.0);
    }
  }
  if (name == "corner_bowler_left") return Vec3(-xc, 0, 0);
  if (name == "corner_bowler_right") return Vec3(xc, 0, 0);
  if (name == "corner_batter_left") return Vec3(-xc, p.length, 0);
  if (name == "corner_batter_right") return Vec3(xc, p.length, 0);
  return std::nullopt;
}

namespace {

constexpr double kDefaultHfovDeg = 65.0;

Mat3 rodrigues(const Vec3& r) {
  const double angle = r.norm();
  if (angle < 1e-12) return Mat3::Identity();
  return Eigen::AngleAxisd(angle, r / angle).toRotationMatrix();
}

struct Problem {
  const CalibrationInput* in;
  std::vector<const TapPoint*> taps;
  double log_f_prior;
  double log_f_sigma;
};

// Parameters: rx ry rz tx ty tz log_f [L]
Camera cameraFrom(const Problem& pb, const Eigen::VectorXd& x) {
  Camera c;
  c.width = pb.in->width;
  c.height = pb.in->height;
  c.cx = 0.5 * c.width;
  c.cy = 0.5 * c.height;
  c.R = rodrigues(x.segment<3>(0));
  c.t = x.segment<3>(3);
  c.f = std::exp(x[6]);
  return c;
}

void residuals(const Problem& pb, const Eigen::VectorXd& x, Eigen::VectorXd& r) {
  const bool est_len = x.size() > 7;
  Pitch pitch = pb.in->pitch;
  if (est_len) pitch.length = x[7];
  const Camera cam = cameraFrom(pb, x);
  r.resize(2 * pb.taps.size() + 1 + (est_len ? 1 : 0));
  Eigen::Index k = 0;
  for (const TapPoint* tp : pb.taps) {
    const Vec3 w = *tapWorldPoint(tp->name, pitch);
    const Vec3 c = cam.toCamera(w);
    if (c.z() < 0.05) {
      r[k++] = 1e3;
      r[k++] = 1e3;
      continue;
    }
    const Vec2 uv = cam.project(w);
    r[k++] = (uv.x() - tp->uv.x()) / pb.in->tap_sigma_px;
    r[k++] = (uv.y() - tp->uv.y()) / pb.in->tap_sigma_px;
  }
  r[k++] = (x[6] - pb.log_f_prior) / pb.log_f_sigma;
  if (est_len) r[k++] = (x[7] - 18.0) / 15.0;  // very weak: keeps the length bounded
}

}  // namespace

CalibrationResult calibrateFromTaps(const CalibrationInput& in) {
  CalibrationResult out;
  if (in.width <= 0 || in.height <= 0) {
    out.error = "image size missing";
    return out;
  }
  Problem pb;
  pb.in = &in;
  std::set<std::string> seen;
  for (const auto& tp : in.taps) {
    if (!tapWorldPoint(tp.name, in.pitch)) {
      out.error = "unknown tap name: " + tp.name;
      return out;
    }
    if (!seen.insert(tp.name).second) {
      out.error = "duplicate tap: " + tp.name;
      return out;
    }
    pb.taps.push_back(&tp);
  }
  for (const char* req : {"batter_left_base", "batter_left_top", "batter_right_base",
                          "batter_right_top"}) {
    if (!seen.count(req)) {
      out.error = std::string("missing required tap: ") + req +
                  " (tap the bottom and top of both outer stumps at the batter's end)";
      return out;
    }
  }
  if (pb.taps.size() < 6) {
    out.error = "need at least 6 taps: the 4 batter-end stump points plus 2 more "
                "(bowler-end stump tops/bottoms or pitch corners)";
    return out;
  }
  const bool has_corners = seen.count("corner_bowler_left") || seen.count("corner_batter_left") ||
                           seen.count("corner_bowler_right") || seen.count("corner_batter_right");

  const double hfov = in.hfov_deg.value_or(kDefaultHfovDeg);
  const double f_prior = 0.5 * in.width / std::tan(0.5 * hfov * kPi / 180.0);
  pb.log_f_prior = std::log(f_prior);
  pb.log_f_sigma = in.hfov_deg ? 0.08 : 0.35;

  const bool est_len = !in.length_known;
  const int n_params = est_len ? 8 : 7;

  // Initialisation: grid over focal length (and length) with SQPnP, then full LM.
  struct Seed {
    double cost;
    Eigen::VectorXd x;
  };
  std::vector<Seed> seeds;
  std::vector<double> lengths = est_len ? std::vector<double>{8, 12, 16, 20, 24}
                                        : std::vector<double>{in.pitch.length};
  for (double L : lengths) {
    Pitch pitch = in.pitch;
    pitch.length = L;
    std::vector<cv::Point3d> obj;
    std::vector<cv::Point2d> img;
    for (const TapPoint* tp : pb.taps) {
      const Vec3 w = *tapWorldPoint(tp->name, pitch);
      obj.emplace_back(w.x(), w.y(), w.z());
      img.emplace_back(tp->uv.x(), tp->uv.y());
    }
    for (int i = 0; i < 24; ++i) {
      const double f = f_prior * std::exp(-1.2 + 2.4 * i / 23.0);
      const cv::Matx33d K(f, 0, 0.5 * in.width, 0, f, 0.5 * in.height, 0, 0, 1);
      cv::Mat rvec, tvec;
      try {
        if (!cv::solvePnP(obj, img, K, cv::noArray(), rvec, tvec, false, cv::SOLVEPNP_SQPNP)) {
          continue;
        }
      } catch (const cv::Exception&) {
        continue;
      }
      Eigen::VectorXd x(n_params);
      x << rvec.at<double>(0), rvec.at<double>(1), rvec.at<double>(2), tvec.at<double>(0),
          tvec.at<double>(1), tvec.at<double>(2), std::log(f);
      if (est_len) x[7] = L;
      Eigen::VectorXd r;
      residuals(pb, x, r);
      seeds.push_back({r.squaredNorm(), x});
    }
  }
  if (seeds.empty()) {
    out.error = "pose solver failed: check the taps";
    return out;
  }
  std::sort(seeds.begin(), seeds.end(), [](const Seed& a, const Seed& b) { return a.cost < b.cost; });

  const ResidualFn fn = [&pb](const Eigen::VectorXd& x, Eigen::VectorXd& r) { residuals(pb, x, r); };
  LMResult best;
  best.cost = std::numeric_limits<double>::infinity();
  for (size_t i = 0; i < std::min<size_t>(4, seeds.size()); ++i) {
    LMOptions opt;
    opt.max_iterations = 200;
    LMResult r = solveLM(fn, seeds[i].x, opt);
    if (r.cost < best.cost) best = r;
  }

  Pitch pitch = in.pitch;
  if (est_len) pitch.length = best.x[7];
  out.camera = cameraFrom(pb, best.x);
  out.pitch_length = pitch.length;
  if (est_len) {
    const Eigen::MatrixXd cov = covarianceFromJacobian(best.jacobian, best.cost);
    out.pitch_length_sigma = std::sqrt(std::max(0.0, cov(7, 7)));
  }

  double sse = 0, maxe = 0;
  for (const TapPoint* tp : pb.taps) {
    const Vec3 w = *tapWorldPoint(tp->name, pitch);
    const double e = (out.camera.project(w) - tp->uv).norm();
    sse += e * e;
    maxe = std::max(maxe, e);
    if (out.camera.depth(w) <= 0) {
      out.error = "calibration put a tapped point behind the camera; check left/right order";
      return out;
    }
  }
  out.rms_px = std::sqrt(sse / pb.taps.size());
  out.max_px = maxe;

  const Vec3 c = out.camera.center();
  std::ostringstream w;
  if (out.rms_px > 2.5) {
    w << "tap reprojection error is high (" << out.rms_px
      << " px rms): re-check the taps (left/right, top/bottom)";
    out.warnings.push_back(w.str());
    w.str("");
  }
  if (c.z() < 0.3 || c.z() > 4.0) {
    w << "camera height " << c.z() << " m looks wrong (expected 0.5-3 m)";
    out.warnings.push_back(w.str());
    w.str("");
  }
  if (c.y() > 0.5) {
    out.warnings.push_back("camera appears to be in front of the bowler's stumps");
  }
  if (est_len && out.pitch_length_sigma > 0.4) {
    w << "pitch length estimated from the stumps is uncertain (" << out.pitch_length << " +/- "
      << out.pitch_length_sigma << " m): please measure it and enter it";
    out.warnings.push_back(w.str());
    w.str("");
  }
  if (!has_corners && !seen.count("bowler_left_top") && !seen.count("bowler_right_top")) {
    out.warnings.push_back(
        "no bowler-end stump tops tapped: focal length relies on the phone's field of view");
  }
  const double f_ratio = out.camera.f / f_prior;
  if (f_ratio < 0.7 || f_ratio > 1.4) {
    w << "focal length differs from the expected field of view by " << (f_ratio - 1) * 100
      << "%; check the taps or the zoom setting";
    out.warnings.push_back(w.str());
  }
  out.ok = true;
  return out;
}

}  // namespace pe
