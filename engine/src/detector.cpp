#include "pocketeye/detector.hpp"

#include <algorithm>
#include <cmath>
#include <opencv2/imgproc.hpp>

namespace pe {

Detector::Detector(const Camera& cam, const Pitch& pitch, double ball_radius, DetectorConfig cfg)
    : cam_(cam), pitch_(pitch), ball_radius_(ball_radius), cfg_(cfg) {
  const double half = 0.5 * pitch.stump_width + cfg.corridor_margin_m;
  const double y0 = std::max(-0.3, cam.center().y() + 0.6);
  box_lo_ = Vec3(-half, y0, 0.0);
  box_hi_ = Vec3(half, pitch.length + 1.5, cfg.corridor_height_m);

  // Corridor mask: projected hull of the corridor box, sampled densely so that
  // parts near/behind the camera plane are handled.
  roi_ = cv::Mat::zeros(cam.height, cam.width, CV_8U);
  std::vector<cv::Point> pts;
  const int n = 12;
  for (int i = 0; i <= n; ++i) {
    for (int j = 0; j <= n; ++j) {
      for (int k = 0; k <= 1; ++k) {
        const Vec3 w(box_lo_.x() + (box_hi_.x() - box_lo_.x()) * i / n,
                     box_lo_.y() + (box_hi_.y() - box_lo_.y()) * j / n,
                     k == 0 ? box_lo_.z() : box_hi_.z());
        if (cam.depth(w) < 0.2) continue;
        const Vec2 uv = cam.project(w);
        pts.emplace_back(static_cast<int>(std::clamp(uv.x(), -1e4, 1e4)),
                         static_cast<int>(std::clamp(uv.y(), -1e4, 1e4)));
      }
    }
  }
  if (pts.size() >= 3) {
    std::vector<cv::Point> hull;
    cv::convexHull(pts, hull);
    cv::fillConvexPoly(roi_, hull, cv::Scalar(255));
  } else {
    roi_.setTo(255);
  }
}

void Detector::reset() {
  frames_.clear();
  times_.clear();
}

std::pair<double, double> Detector::expectedRadius(const Vec2& uv) const {
  const Vec3 c = cam_.center();
  const Vec3 d = cam_.rayDirection(uv);
  const auto hit = intersectBox(c, d, box_lo_, box_hi_);
  if (!hit) return {0.0, 0.0};
  const double depth_scale = (cam_.R * d).z();
  const double s0 = std::max(hit->first, 0.3);
  const double s1 = std::max(hit->second, s0);
  return {cam_.f * ball_radius_ / (s1 * depth_scale), cam_.f * ball_radius_ / (s0 * depth_scale)};
}

bool Detector::push(const cv::Mat& gray, double t, std::vector<Candidate>& out, double& t_mid) {
  CV_Assert(gray.type() == CV_8UC1);
  frames_.push_back(gray.clone());
  times_.push_back(t);
  if (frames_.size() > 3) {
    frames_.pop_front();
    times_.pop_front();
  }
  out.clear();
  if (frames_.size() < 3) return false;
  t_mid = times_[1];
  detect(frames_[0], frames_[1], frames_[2], out);
  return true;
}

void Detector::detect(const cv::Mat& prev, const cv::Mat& cur, const cv::Mat& next,
                      std::vector<Candidate>& out) {
  cv::absdiff(cur, prev, d1_);
  cv::absdiff(cur, next, d2_);
  cv::min(d1_, d2_, diff_);
  diff_.setTo(0, roi_ == 0);

  // Robust noise level from a subsampled histogram of the corridor.
  int hist[256] = {0};
  int count = 0;
  for (int y = 0; y < diff_.rows; y += 4) {
    const uchar* row = diff_.ptr<uchar>(y);
    const uchar* m = roi_.ptr<uchar>(y);
    for (int x = 0; x < diff_.cols; x += 4) {
      if (!m[x]) continue;
      ++hist[row[x]];
      ++count;
    }
  }
  int median = 0;
  for (int acc = 0; median < 255 && acc + hist[median] < count / 2; ++median) acc += hist[median];
  const double sigma = 1.4826 * std::max(median, 1);
  const double thr = std::max(cfg_.min_threshold, cfg_.noise_k * sigma);

  cv::threshold(diff_, bin_, thr, 255, cv::THRESH_BINARY);
  cv::morphologyEx(bin_, bin_, cv::MORPH_CLOSE,
                   cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(3, 3)));
  const int n = cv::connectedComponentsWithStats(bin_, labels_, stats_, centroids_, 8, CV_32S);
  if (n - 1 > cfg_.max_blobs) return;

  for (int i = 1; i < n; ++i) {
    const int area = stats_.at<int>(i, cv::CC_STAT_AREA);
    if (area < 3) continue;
    const int bx = stats_.at<int>(i, cv::CC_STAT_LEFT);
    const int by = stats_.at<int>(i, cv::CC_STAT_TOP);
    const int bw = stats_.at<int>(i, cv::CC_STAT_WIDTH);
    const int bh = stats_.at<int>(i, cv::CC_STAT_HEIGHT);
    if (bw > cam_.width / 4 || bh > cam_.height / 4) continue;

    // Difference-weighted moments over the component.
    double sw = 0, sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
    for (int y = by; y < by + bh; ++y) {
      const int* lab = labels_.ptr<int>(y);
      const uchar* d = diff_.ptr<uchar>(y);
      for (int x = bx; x < bx + bw; ++x) {
        if (lab[x] != i) continue;
        const double w = d[x] > 0 ? d[x] : 1.0;
        sw += w;
        sx += w * x;
        sy += w * y;
        sxx += w * x * x;
        syy += w * y * y;
        sxy += w * x * y;
      }
    }
    if (sw <= 0) continue;
    const double mx = sx / sw, my = sy / sw;
    const double cxx = sxx / sw - mx * mx + 1.0 / 12.0;
    const double cyy = syy / sw - my * my + 1.0 / 12.0;
    const double cxy = sxy / sw - mx * my;
    const double tr = cxx + cyy;
    const double det = cxx * cyy - cxy * cxy;
    const double disc = std::sqrt(std::max(0.0, tr * tr / 4 - det));
    const double l_max = tr / 2 + disc;
    const double l_min = std::max(tr / 2 - disc, 1e-3);
    // Uniform disc of radius r: variance r^2 / 4 along any axis.
    const double radius = 2.0 * std::sqrt(l_min);
    const double elong = std::sqrt(l_max / l_min);
    if (elong > cfg_.max_elongation) continue;

    const Vec2 uv(mx, my);
    const auto [rmin, rmax] = expectedRadius(uv);
    if (rmax <= 0) continue;
    if (radius < 0.4 * rmin - 1.0 || radius > 1.6 * rmax + 1.5) continue;
    const double max_area = kPi * std::pow(1.6 * rmax + 2.0, 2) * std::min(elong, 6.0);
    if (area > max_area) continue;

    Candidate c;
    c.uv = uv;
    c.radius_px = radius;
    c.area = area;
    c.elongation = elong;
    c.strength = sw / area;
    out.push_back(c);
  }
  std::sort(out.begin(), out.end(),
            [](const Candidate& a, const Candidate& b) { return a.strength > b.strength; });
  if (static_cast<int>(out.size()) > cfg_.max_candidates) out.resize(cfg_.max_candidates);
}

}  // namespace pe
