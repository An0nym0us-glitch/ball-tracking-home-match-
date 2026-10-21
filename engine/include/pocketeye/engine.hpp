#pragma once

#include <deque>
#include <memory>
#include <opencv2/core.hpp>
#include <vector>

#include "pocketeye/analysis.hpp"
#include "pocketeye/detector.hpp"
#include "pocketeye/tracker.hpp"

namespace pe {

struct EngineConfig {
  AnalysisConfig analysis;
  DetectorConfig detector;
  PathConfig tracker;
  size_t min_track_points = 7;
  double hold_s = 0.6;  // keep a delivery this long (video time) to merge duplicate tracks
  bool keep_rejected = false;  // also report tracks that were not deliveries (debugging)
  bool skip_analysis = false;  // debugging: report raw tracks only
};

// Frames in, deliveries out. Every delivery is detected automatically: no
// "record" button per ball. Used identically by the CLI, the laptop app and
// the phone-only mode.
class Engine {
 public:
  explicit Engine(const EngineConfig& cfg);

  // Feed one frame with its capture time in seconds, in order: 8-bit luma
  // (CV_8UC1) or BGR (CV_8UC3). Colour finds the ball against similar-brightness
  // backgrounds (white pads, sky) far more reliably.
  void pushFrame(const cv::Mat& image, double t);
  // End of stream: closes open tracks and analyses them.
  void finish();
  // Next finished delivery (valid, or rejected when keep_rejected is set).
  bool popResult(DeliveryResult& out);

  struct Stats {
    int frames = 0;
    double detect_ms = 0.0;
    double track_ms = 0.0;
    double analyze_ms = 0.0;
    int tracks_analyzed = 0;
    int deliveries = 0;
  };
  const Stats& stats() const { return stats_; }
  const EngineConfig& config() const { return cfg_; }
  const std::vector<Candidate>& lastCandidates() const { return last_candidates_; }
  int lastCandidateFrame() const { return frame_index_ - 1; }

 private:
  void analyzeFinished();
  void emitPending(double t_now, bool all);

  EngineConfig cfg_;
  Detector detector_;
  Tracker tracker_;
  int frame_index_ = 0;  // index of the next frame pushed
  std::vector<Candidate> last_candidates_;
  std::deque<std::pair<int, double>> detect_ms_per_frame_;
  std::deque<DeliveryResult> results_;
  std::vector<DeliveryResult> pending_;  // valid deliveries waiting for duplicates to close
  double t_now_ = 0.0;
  Stats stats_;
};

}  // namespace pe
