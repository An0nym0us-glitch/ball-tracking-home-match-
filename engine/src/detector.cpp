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
  cv::Rect r = cv::boundingRect(roi_);
  if (r.area() == 0) r = cv::Rect(0, 0, cam.width, cam.height);
  // Even coordinates so that the half-resolution chroma lines up.
  const int x0 = r.x & ~1, y0e = r.y & ~1;
  const int x1 = std::min(cam.width & ~1, (r.x + r.width + 1) & ~1);
  const int y1 = std::min(cam.height & ~1, (r.y + r.height + 1) & ~1);
  roi_rect_ = cv::Rect(x0, y0e, std::max(2, x1 - x0), std::max(2, y1 - y0e));
  roi_crop_ = roi_(roi_rect_);
  label_scale_ = static_cast<double>(cam.width) * cam.height > 1.5e6 ? 2 : 1;
}

void toYuvFrame(const cv::Mat& image, YuvFrame& out) {
  CV_Assert(image.type() == CV_8UC1 || image.type() == CV_8UC3);
  if (image.type() == CV_8UC1) {
    image.copyTo(out.y);
    out.uv.release();
    return;
  }
  cv::Mat yuv;
  cv::cvtColor(image, yuv, cv::COLOR_BGR2YUV);
  cv::Mat ch[3];
  cv::split(yuv, ch);
  out.y = ch[0];
  cv::Mat uv;
  cv::merge(std::vector<cv::Mat>{ch[1], ch[2]}, uv);
  cv::resize(uv, out.uv, cv::Size(image.cols / 2, image.rows / 2), 0, 0, cv::INTER_AREA);
}

void Detector::reset() { count_ = 0; }

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

bool Detector::push(const cv::Mat& image, double t, std::vector<Candidate>& out, double& t_mid) {
  toYuvFrame(image, converted_);
  return push(converted_, t, out, t_mid);
}

bool Detector::push(const YuvFrame& frame, double t, std::vector<Candidate>& out, double& t_mid) {
  CV_Assert(frame.y.type() == CV_8UC1 && frame.y.cols == cam_.width && frame.y.rows == cam_.height);
  // Rotate the ring: slot 2 is the newest. Only the corridor is kept.
  std::swap(ring_[0], ring_[1]);
  std::swap(ring_[1], ring_[2]);
  frame.y(roi_rect_).copyTo(ring_[2].y);
  const cv::Rect rc(roi_rect_.x / 2, roi_rect_.y / 2, roi_rect_.width / 2, roi_rect_.height / 2);
  if (!frame.uv.empty() && frame.uv.type() == CV_8UC2 && frame.uv.cols >= rc.x + rc.width &&
      frame.uv.rows >= rc.y + rc.height) {
    frame.uv(rc).copyTo(ring_[2].uv);
  } else {
    ring_[2].uv.release();
  }
  times_[0] = times_[1];
  times_[1] = times_[2];
  times_[2] = t;
  count_ = std::min(count_ + 1, 3);
  out.clear();
  if (count_ < 3) return false;
  t_mid = times_[1];
  detect(ring_[0], ring_[1], ring_[2], out);
  return true;
}

int Detector::labelBlobs(double thr) {
  cv::threshold(diff_, bin_, thr, 255, cv::THRESH_BINARY);
  if (label_scale_ == 1) {
    cv::morphologyEx(bin_, bin_, cv::MORPH_CLOSE, cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(3, 3)));
    return cv::connectedComponentsWithStats(bin_, labels_, stats_, centroids_, 8, CV_32S);
  }
  // Large frames: label a half-size map (a 2x2 cell is set if any pixel is),
  // which also closes 1-pixel gaps like the closing above, at a quarter of
  // the cost. Blob positions and sizes are still measured at full resolution.
  bin_half_.create((bin_.rows + 1) / 2, (bin_.cols + 1) / 2, CV_8U);
  cv::parallel_for_(cv::Range(0, bin_half_.rows), [&](const cv::Range& r) {
    for (int hy = r.start; hy < r.end; ++hy) {
      const uchar* a = bin_.ptr<uchar>(2 * hy);
      const uchar* b = bin_.ptr<uchar>(std::min(2 * hy + 1, bin_.rows - 1));
      uchar* o = bin_half_.ptr<uchar>(hy);
      for (int hx = 0; hx < bin_half_.cols; ++hx) {
        const int x0 = 2 * hx, x1 = std::min(2 * hx + 1, bin_.cols - 1);
        o[hx] = a[x0] | a[x1] | b[x0] | b[x1];
      }
    }
  });
  return cv::connectedComponentsWithStats(bin_half_, labels_, stats_, centroids_, 8, CV_32S);
}

// Works inside roi_rect_: blob coordinates are shifted back at the end.
void Detector::detect(const YuvFrame& prev, const YuvFrame& cur, const YuvFrame& next,
                      std::vector<Candidate>& out) {
  // Three-frame difference, one pass over the corridor split across the CPU
  // cores (the plain OpenCV operations run on one core): brightness at full
  // resolution, colour at half resolution (as the camera delivers it). Colour
  // helps a lot: a yellow ball in front of white pads has little brightness
  // contrast but a large U (blue-difference) change.
  const bool colour = !cur.uv.empty() && !prev.uv.empty() && !next.uv.empty();
  diff_.create(cur.y.size(), CV_8U);
  uchar gain[256];
  for (int v = 0; v < 256; ++v) gain[v] = cv::saturate_cast<uchar>(v * cfg_.chroma_gain);
  const int rows = diff_.rows, cols = diff_.cols;
  cv::parallel_for_(cv::Range(0, (rows + 1) / 2), [&](const cv::Range& r) {
    const int hc_n = (cols + 1) / 2;
    std::vector<uchar> ca(hc_n + 1, 0), cb(hc_n + 1, 0), cmid(hc_n + 1, 0), up(2 * hc_n + 2, 0);
    // Colour difference of one half-resolution row (scaled by the gain).
    auto colourRow = [&](int hy, std::vector<uchar>& out) {
      std::fill(out.begin(), out.end(), 0);
      if (!colour || hy >= cur.uv.rows) return;
      const uchar *c = cur.uv.ptr<uchar>(hy), *p = prev.uv.ptr<uchar>(hy), *n = next.uv.ptr<uchar>(hy);
      const int hc = std::min(cur.uv.cols, hc_n);
      for (int x = 0; x < hc; ++x) {
        const int k = 2 * x;
        const int du = std::min(std::abs(c[k] - p[k]), std::abs(c[k] - n[k]));
        const int dv = std::min(std::abs(c[k + 1] - p[k + 1]), std::abs(c[k + 1] - n[k + 1]));
        out[x] = gain[std::max(du, dv)];
      }
      out[hc] = hc > 0 ? out[hc - 1] : 0;
    };
    if (colour) colourRow(r.start, ca);
    for (int hy = r.start; hy < r.end; ++hy) {
      if (colour) {
        colourRow(hy + 1, cb);
        for (int x = 0; x <= hc_n; ++x) cmid[x] = static_cast<uchar>((ca[x] + cb[x] + 1) >> 1);
      }
      for (int y = 2 * hy; y < std::min(rows, 2 * hy + 2); ++y) {
        const uchar *c = cur.y.ptr<uchar>(y), *p = prev.y.ptr<uchar>(y), *n = next.y.ptr<uchar>(y);
        const uchar* m = roi_crop_.ptr<uchar>(y);
        uchar* d = diff_.ptr<uchar>(y);
        for (int x = 0; x < cols; ++x) {
          const uchar a = c[x] > p[x] ? c[x] - p[x] : p[x] - c[x];
          const uchar b = c[x] > n[x] ? c[x] - n[x] : n[x] - c[x];
          d[x] = std::min(a, b);
        }
        if (colour) {
          // Colour at half resolution, interpolated: even rows/columns sit on a
          // sample, odd ones half-way between two.
          const uchar* cr = ((y & 1) ? cmid : ca).data();
          uchar* u = up.data();
          for (int hx = 0; hx < hc_n; ++hx) {
            u[2 * hx] = cr[hx];
            u[2 * hx + 1] = static_cast<uchar>((cr[hx] + cr[hx + 1] + 1) >> 1);
          }
          for (int x = 0; x < cols; ++x) d[x] = std::max(d[x], u[x]);
        }
        for (int x = 0; x < cols; ++x) d[x] &= m[x];  // corridor mask (0 / 255)
      }
      if (colour) ca.swap(cb);
    }
  });

  // Robust noise level from a subsampled histogram of the corridor.
  int hist[256] = {0};
  int count = 0;
  for (int y = 0; y < diff_.rows; y += 4) {
    const uchar* row = diff_.ptr<uchar>(y);
    const uchar* m = roi_crop_.ptr<uchar>(y);
    for (int x = 0; x < diff_.cols; x += 4) {
      if (!m[x]) continue;
      ++hist[row[x]];
      ++count;
    }
  }
  int median = 0;
  for (int acc = 0; median < 255 && acc + hist[median] < count / 2; ++median) acc += hist[median];
  const double sigma = 1.4826 * std::max(median, 1);
  double thr = std::max(cfg_.min_threshold, cfg_.noise_k * sigma);

  int n = labelBlobs(thr);
  // Too many blobs: a sudden change (exposure, shake, wind in leaves). A ball
  // is usually still among the strongest changes, so look again with a higher
  // threshold before giving up on the frame.
  for (int retry = 0; retry < 2 && n - 1 > cfg_.max_blobs; ++retry) {
    thr *= 2.0;
    n = labelBlobs(thr);
  }
  if (n - 1 > cfg_.max_blobs) return;

  // Labels may be at half resolution (label_scale_ 2): sizes are in label pixels.
  const int ls = label_scale_;
  // Very large moving regions are people close to the camera (the bowler): small
  // blobs right next to them are fragments of their outline, not the ball. The
  // batter is far away and small, so blobs near the pads are kept.
  const double big_area = cfg_.big_region_frac * cam_.width * cam_.height;
  big_mask_ = cv::Mat::zeros(labels_.size(), CV_8U);
  bool any_big = false;
  for (int i = 1; i < n; ++i) {
    if (stats_.at<int>(i, cv::CC_STAT_AREA) * ls * ls > big_area) {
      big_mask_.setTo(255, labels_ == i);
      any_big = true;
    }
  }
  if (any_big) {
    const int m = std::max(1, cfg_.big_region_margin_px / ls);
    cv::dilate(big_mask_, big_mask_, cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(2 * m + 1, 2 * m + 1)));
  }

  for (int i = 1; i < n; ++i) {
    if (stats_.at<int>(i, cv::CC_STAT_AREA) * ls * ls < cfg_.min_area) continue;
    const int bx = stats_.at<int>(i, cv::CC_STAT_LEFT) * ls;
    const int by = stats_.at<int>(i, cv::CC_STAT_TOP) * ls;
    const int bw = std::min(stats_.at<int>(i, cv::CC_STAT_WIDTH) * ls, diff_.cols - bx);
    const int bh = std::min(stats_.at<int>(i, cv::CC_STAT_HEIGHT) * ls, diff_.rows - by);
    if (bw > cam_.width / 4 || bh > cam_.height / 4) continue;

    // Difference-weighted moments over the component (full resolution). With
    // half-size labels, a cell's pixels count only if they changed themselves.
    double sw = 0, sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
    int area = 0;
    const double keep = ls == 1 ? 0.0 : 0.5 * thr;
    for (int y = by; y < by + bh; ++y) {
      const int* lab = labels_.ptr<int>(y / ls);
      const uchar* d = diff_.ptr<uchar>(y);
      for (int x = bx; x < bx + bw; ++x) {
        if (lab[x / ls] != i || d[x] < keep) continue;
        ++area;
        const double w = d[x] > 0 ? d[x] : 1.0;
        sw += w;
        sx += w * x;
        sy += w * y;
        sxx += w * x * x;
        syy += w * y * y;
        sxy += w * x * y;
      }
    }
    if (sw <= 0 || area < cfg_.min_area) continue;
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

    if (any_big && big_mask_.at<uchar>(std::clamp(static_cast<int>(my) / ls, 0, big_mask_.rows - 1),
                                       std::clamp(static_cast<int>(mx) / ls, 0, big_mask_.cols - 1))) {
      continue;
    }
    const Vec2 uv(mx + roi_rect_.x, my + roi_rect_.y);
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
