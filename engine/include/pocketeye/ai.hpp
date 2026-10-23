#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <opencv2/core.hpp>
#include <string>
#include <thread>
#include <vector>

#include "pocketeye/geometry.hpp"

namespace pe {

// AI assist: a YOLO object detector (YOLOX, COCO classes, ONNX) on its own
// thread, on the CPU or a GPU. It cannot find a cricket ball in a whole frame
// (4-10 px, smaller than anything it was trained on); it is used for:
// - people (bowler, batter) in a small copy of the frame, a few times a second;
// - "is this a ball?" on a small crop around a moving spot, enlarged to the
//   model input (a 6-24 px ball is found there, docs/accuracy.md).
struct AiConfig {
  bool enabled = false;
  std::string model;            // YOLOX ONNX file (COCO): yolox_nano.onnx finds 6-24 px balls in a crop best
  std::string device = "dgpu";  // dgpu (dedicated GPU), igpu (integrated GPU) or cpu
  int input = 416;              // model input size (px, square)
  double person_hz = 4.0;       // people detections per second
  float ball_score = 0.3f;      // "sports ball" score that counts as a ball
  int crop_px = 104;            // crop around a spot (at least), enlarged to the model input
};

struct AiBox {
  cv::Rect2f box;  // picture pixels
  float score = 0.0f;
};

// A crop to check for a ball.
struct AiCheck {
  int id = 0;
  double t = 0.0;
  Vec2 uv = Vec2::Zero();  // crop centre (picture pixels)
};
struct AiCheckResult {
  int id = 0;
  double t = 0.0;
  float score = 0.0f;      // best "sports ball" score in the crop
  Vec2 uv = Vec2::Zero();  // its centre (picture pixels)
};

struct AiStatus {
  bool ready = false;   // model loaded
  std::string device;   // what it runs on, e.g. "NVIDIA GeForce RTX 3050 (OpenCL)" or "CPU"
  std::string note;     // e.g. why another device than the one asked for is used
  std::string error;    // model could not be loaded
  double person_ms = 0.0;  // last people detection
  double check_ms = 0.0;   // last ball check
  int checks = 0;
};

// YOLOX output (1 x N x 85, raw grid predictions) -> boxes of one COCO class in
// input pixels, after non-maximum suppression.
std::vector<AiBox> decodeYolox(const cv::Mat& out, int input, int cls, float min_score);

class AiAssist {
 public:
  explicit AiAssist(AiConfig cfg);
  ~AiAssist();
  AiAssist(const AiAssist&) = delete;
  AiAssist& operator=(const AiAssist&) = delete;

  const AiConfig& config() const { return cfg_; }
  // True when it is time for the next people detection.
  bool wantsPeople(double t) const;
  // A copy of the frame (BGR, any size) and its scale to picture pixels. Replaces a waiting one.
  void submitPeople(double t, cv::Mat bgr, double to_picture);
  // Crops (BGR, crop_px square, centred on check.uv). The oldest are dropped if too many wait.
  void submitCheck(const AiCheck& check, cv::Mat bgr);
  // Latest people (picture pixels) and their time; false if none yet.
  bool people(std::vector<AiBox>& out, double& t) const;
  // Finished ball checks since the last call.
  std::vector<AiCheckResult> takeChecks();
  AiStatus status() const;
  // Runs the model on one picture (any size, letterboxed): boxes of a class in its pixels. Blocking.
  std::vector<AiBox> detectNow(const cv::Mat& bgr, int cls, float min_score);

  static constexpr int kPerson = 0;
  static constexpr int kSportsBall = 32;

 private:
  void worker();
  std::vector<AiBox> run(const cv::Mat& bgr, int cls, float min_score);  // worker thread only
  bool load();  // worker thread only: picks the device, loads the model

  AiConfig cfg_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  bool stop_ = false;
  cv::Mat people_in_;
  double people_t_ = 0.0, people_scale_ = 1.0;
  bool people_pending_ = false;
  double people_sent_t_ = -1e9;
  std::deque<std::pair<AiCheck, cv::Mat>> checks_in_;
  std::vector<AiCheckResult> checks_out_;
  std::vector<AiBox> people_out_;
  double people_out_t_ = -1.0;
  AiStatus status_;
  std::deque<std::function<void()>> calls_;  // detectNow: run on the worker thread
  struct Net;
  Net* net_ = nullptr;  // owned by the worker thread (the GPU context is bound to it)
  std::thread thread_;
};

}  // namespace pe
