#pragma once

#include <deque>
#include <opencv2/core.hpp>
#include <utility>
#include <vector>

#include "pocketeye/geometry.hpp"

namespace pe {

struct Candidate {
  Vec2 uv = Vec2::Zero();  // blob centre (px)
  double radius_px = 0.0;  // from the blob's minor axis (motion blur stretches the major axis)
  double area = 0.0;
  double elongation = 1.0;  // major / minor axis
  double strength = 0.0;    // mean frame difference inside the blob
};

struct DetectorConfig {
  double min_threshold = 10.0;   // minimum grey-level difference
  double noise_k = 5.0;          // threshold = max(min_threshold, noise_k * noise sigma)
  int max_candidates = 16;       // per frame, strongest first
  int max_blobs = 300;           // more blobs than this = global change (exposure jump, shake)
  double corridor_margin_m = 0.9;  // around the stumps line, each side
  double corridor_height_m = 2.8;
  double max_elongation = 10.0;
};

// Moving-ball candidates from a three-frame difference inside the pitch corridor.
// Candidates for frame k are produced when frame k+1 arrives.
class Detector {
 public:
  Detector(const Camera& cam, const Pitch& pitch, double ball_radius, DetectorConfig cfg = {});

  // Returns true when candidates for the previous frame are ready (t_mid = its time).
  bool push(const cv::Mat& gray, double t, std::vector<Candidate>& out, double& t_mid);
  void reset();

  const cv::Mat& corridorMask() const { return roi_; }
  // Plausible apparent radius range (px) for a ball seen at pixel uv inside the corridor.
  std::pair<double, double> expectedRadius(const Vec2& uv) const;

 private:
  void detect(const cv::Mat& prev, const cv::Mat& cur, const cv::Mat& next,
              std::vector<Candidate>& out);

  Camera cam_;
  Pitch pitch_;
  double ball_radius_;
  DetectorConfig cfg_;
  cv::Mat roi_;
  Vec3 box_lo_, box_hi_;
  std::deque<cv::Mat> frames_;
  std::deque<double> times_;
  cv::Mat d1_, d2_, diff_, bin_, labels_, stats_, centroids_;
};

}  // namespace pe
