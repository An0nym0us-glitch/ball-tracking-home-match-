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
  double max_elongation = 16.0;  // motion blur: a fast ball is a streak at 1/60-1/125 s
  int min_area = 4;               // px
  int big_region_margin_px = 12;  // ignore small blobs this close to large moving regions
  double big_region_frac = 0.015;  // "large" = more than this fraction of the image
  double chroma_gain = 2.0;       // colour (U/V) differences are about half the size of B/G/R ones
  int presmooth = 0;              // Gaussian kernel (px, odd) on brightness before differencing; 0 = none
};

// Frames that were each compressed as a separate JPEG (the old-phone camera
// app streaming to the laptop): every frame carries fresh 8x8-block noise,
// strongest in the colour channels. Smoothing brightness a little and giving
// colour normal weight finds more balls there (docs/accuracy.md); on video
// straight from the camera it does not help, so it is only used for such streams.
DetectorConfig forJpegStream(DetectorConfig cfg);

// A frame as cameras deliver it: full-resolution luma and half-resolution
// interleaved chroma (CV_8UC2, U then V; empty for grey video).
struct YuvFrame {
  cv::Mat y;
  cv::Mat uv;
};
// BGR (CV_8UC3) or grey (CV_8UC1) image -> YuvFrame (reusing out's buffers).
void toYuvFrame(const cv::Mat& image, YuvFrame& out);

// Moving-ball candidates from a three-frame difference inside the pitch corridor.
// Candidates for frame k are produced when frame k+1 arrives.
class Detector {
 public:
  Detector(const Camera& cam, const Pitch& pitch, double ball_radius, DetectorConfig cfg = {});

  // Feed frames in order. Returns true when the candidates for the previous
  // frame are ready (t_mid = its time).
  bool push(const YuvFrame& frame, double t, std::vector<Candidate>& out, double& t_mid);
  // 8-bit luma (CV_8UC1) or BGR (CV_8UC3).
  bool push(const cv::Mat& image, double t, std::vector<Candidate>& out, double& t_mid);
  void reset();

  const cv::Mat& corridorMask() const { return roi_; }
  // Plausible apparent radius range (px) for a ball seen at pixel uv inside the corridor.
  std::pair<double, double> expectedRadius(const Vec2& uv) const;

 private:
  void detect(const YuvFrame& prev, const YuvFrame& cur, const YuvFrame& next, std::vector<Candidate>& out);
  int labelBlobs(double thr);

  Camera cam_;
  Pitch pitch_;
  double ball_radius_;
  DetectorConfig cfg_;
  cv::Mat roi_;
  cv::Rect roi_rect_;  // bounding box of the corridor (even coordinates); work is done inside it
  cv::Mat roi_crop_;   // roi_(roi_rect_)
  Vec3 box_lo_, box_hi_;
  YuvFrame ring_[3];   // the last three frames, oldest first after rotation
  double times_[3] = {0, 0, 0};
  int count_ = 0;
  YuvFrame converted_;
  cv::Mat diff_, bin_, bin_half_, labels_, stats_, centroids_;
  int label_scale_ = 1;  // 2: blobs are labelled at half resolution (large frames)
  cv::Mat big_mask_;
};

}  // namespace pe
