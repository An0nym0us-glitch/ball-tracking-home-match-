#include "overlay.hpp"

#include <cmath>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>
#include <sstream>

namespace pe {

namespace {

cv::Point2f px(const Camera& cam, const Vec3& p) {
  const Vec2 uv = cam.project(p);
  return {static_cast<float>(uv.x()), static_cast<float>(uv.y())};
}

void drawStumps(cv::Mat& img, const AnalysisConfig& cfg) {
  const Camera& cam = cfg.camera;
  const double xs = cfg.pitch.outerStumpX();
  for (double y : {0.0, cfg.pitch.length}) {
    for (double x : {-xs, 0.0, xs}) {
      const Vec3 base(x, y, 0), top(x, y, cfg.pitch.stump_height);
      if (cam.depth(base) < 0.3 || cam.depth(top) < 0.3) continue;
      cv::line(img, px(cam, base), px(cam, top), cv::Scalar(255, 255, 255), 1, cv::LINE_AA);
    }
  }
}

std::string num(double v, int prec = 0) {
  std::ostringstream s;
  s.setf(std::ios::fixed);
  s.precision(prec);
  s << v;
  return s.str();
}

void drawCard(cv::Mat& img, const DeliveryResult& r) {
  std::vector<std::string> lines;
  if (r.release_speed_kmh.valid) lines.push_back("Speed " + num(r.release_speed_kmh.value) + " km/h");
  lines.push_back("Length: " + r.length_zone + (r.line_zone.empty() ? "" : ", " + r.line_zone));
  if (r.lbw_applicable) {
    lines.push_back("Pitching: " + r.lbw.pitching.status);
    lines.push_back("Impact:   " + r.lbw.impact.status);
    lines.push_back("Wickets:  " + r.lbw.wickets.status);
    lines.push_back("LBW: " + r.lbw.decision);
  } else if (r.reaches_stumps) {
    lines.push_back("Stumps: " + r.lbw.wickets.status);
  }
  const int x = 16, y0 = 16, lh = 26;
  cv::rectangle(img, cv::Rect(x - 8, y0 - 6, 330, lh * static_cast<int>(lines.size()) + 12),
                cv::Scalar(0, 0, 0), cv::FILLED);
  for (size_t i = 0; i < lines.size(); ++i) {
    cv::Scalar col(255, 255, 255);
    if (lines[i].rfind("LBW: ", 0) == 0) {
      col = r.lbw.decision == "OUT" ? cv::Scalar(60, 60, 255)
                                    : (r.lbw.decision == "NOT OUT" ? cv::Scalar(80, 220, 80)
                                                                   : cv::Scalar(0, 200, 255));
    }
    cv::putText(img, lines[i], cv::Point(x, y0 + 18 + lh * static_cast<int>(i)),
                cv::FONT_HERSHEY_SIMPLEX, 0.6, col, 1, cv::LINE_AA);
  }
}

void drawDelivery(cv::Mat& img, const DeliveryResult& r, const AnalysisConfig& cfg, double t) {
  const Camera& cam = cfg.camera;
  for (size_t i = 0; i < r.observations.size(); ++i) {
    const Observation& o = r.observations[i];
    if (o.t > t + 1e-6) break;
    const cv::Scalar col = r.labels[i] == kOutlier      ? cv::Scalar(128, 128, 128)
                           : r.labels[i] == kAfterImpact ? cv::Scalar(255, 0, 255)
                                                         : cv::Scalar(0, 255, 255);
    cv::circle(img, cv::Point2f(o.uv.x(), o.uv.y()), std::max(3, static_cast<int>(o.radius_px + 2)), col, 1,
               cv::LINE_AA);
  }
  std::vector<cv::Point2f> pts;
  for (const auto& [tp, p] : r.path) {
    if (tp > t) break;
    if (cam.depth(p) > 0.3) pts.push_back(px(cam, p));
  }
  for (size_t i = 1; i < pts.size(); ++i) cv::line(img, pts[i - 1], pts[i], cv::Scalar(0, 140, 255), 2, cv::LINE_AA);
  if (r.has_bounce && t >= r.t_bounce && !r.path.empty()) {
    for (const auto& [tp, p] : r.path) {
      if (tp >= r.t_bounce) {
        cv::circle(img, px(cam, Vec3(p.x(), p.y(), 0)), 6, cv::Scalar(0, 255, 0), 2, cv::LINE_AA);
        break;
      }
    }
  }
  if (std::isfinite(r.t_impact) && t >= r.t_impact) {
    cv::drawMarker(img, px(cam, r.impact_point), cv::Scalar(255, 0, 255), cv::MARKER_TILTED_CROSS, 12, 2);
    for (size_t i = 1; i < r.projection.size(); i += 2) {
      cv::line(img, px(cam, r.projection[i - 1].second), px(cam, r.projection[i].second),
               cv::Scalar(255, 255, 0), 2, cv::LINE_AA);
    }
  }
  const double t_card = std::isfinite(r.t_impact) ? std::min(r.t_last, r.t_impact + 0.3) : r.t_last;
  if (t >= t_card) drawCard(img, r);
}

}  // namespace

bool writeOverlayVideo(const std::string& video_in, const std::vector<double>& timestamps,
                       const std::vector<DeliveryResult>& deliveries, const AnalysisConfig& cfg,
                       const std::string& video_out, std::string* error) {
  cv::VideoCapture cap(video_in);
  if (!cap.isOpened()) {
    *error = "cannot reopen " + video_in;
    return false;
  }
  double fps = cap.get(cv::CAP_PROP_FPS);
  if (!(fps > 1 && fps < 1000)) fps = 30;
  cv::VideoWriter out(video_out, cv::VideoWriter::fourcc('m', 'p', '4', 'v'), fps,
                      cv::Size(cfg.camera.width, cfg.camera.height));
  if (!out.isOpened()) {
    *error = "cannot write " + video_out;
    return false;
  }
  cv::Mat frame;
  for (size_t i = 0; cap.read(frame) && i < timestamps.size(); ++i) {
    if (frame.channels() == 1) cv::cvtColor(frame, frame, cv::COLOR_GRAY2BGR);
    const double t = timestamps[i];
    drawStumps(frame, cfg);
    for (const DeliveryResult& r : deliveries) {
      if (t >= r.t_first - 1e-6 && t <= r.t_last + 2.5) drawDelivery(frame, r, cfg, t);
    }
    out.write(frame);
  }
  return true;
}

}  // namespace pe
