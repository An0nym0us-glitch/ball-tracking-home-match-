#pragma once

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <opencv2/core.hpp>
#include <vector>

#include "pocketeye/analysis.hpp"
#include "pocketeye/detector.hpp"
#include "pocketeye/live.hpp"
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
  // Analyse finished tracks on a background thread so that frame processing
  // never stalls (an analysis takes ~0.5-3 s on a phone). Same results either way.
  bool async_analysis = true;
};

// Frames in, deliveries out. Every delivery is detected automatically: no
// "record" button per ball. Used identically by the CLI, the laptop app and
// the phone-only mode.
class Engine {
 public:
  explicit Engine(const EngineConfig& cfg);
  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  // Feed one frame with its capture time in seconds, in order: 8-bit luma
  // (CV_8UC1) or BGR (CV_8UC3). Colour finds the ball against similar-brightness
  // backgrounds (white pads, sky) far more reliably.
  void pushFrame(const cv::Mat& image, double t);
  // Camera frame as delivered (luma + half-resolution chroma): no colour conversion.
  void pushFrame(const YuvFrame& frame, double t);
  // End of stream: closes open tracks, analyses them and waits for all analyses.
  void finish();
  // Next finished delivery (valid, or rejected when keep_rejected is set).
  // Also picks up analyses that finished on the background thread since the
  // last frame, so results appear even if the stream pauses.
  bool popResult(DeliveryResult& out);

  struct Stats {
    int frames = 0;
    double detect_ms = 0.0;
    double track_ms = 0.0;
    double analyze_ms = 0.0;
    int tracks_analyzed = 0;
    int deliveries = 0;
    int rejected = 0;            // tracks analysed that were not deliveries
    std::string last_reject;     // why the latest one was rejected
    int analyses_waiting = 0;    // queued or running on the background thread
  };
  // Snapshot (the background thread updates some fields).
  Stats stats() const;
  const EngineConfig& config() const { return cfg_; }
  const std::vector<Candidate>& lastCandidates() const { return last_candidates_; }
  int lastCandidateFrame() const { return frame_index_ - 1; }
  // Outline of the watched corridor in the picture (for a debugging view).
  std::vector<Vec2> corridorOutline() const;
  // The ball being followed right now, for drawing on the live picture.
  LiveBall liveBall() const { return follower_.state(t_now_); }

 private:
  struct Job {
    std::vector<TrackPoint> track;
    double detect_ms = 0;
    int close_frame = 0;
  };
  void queueFinished();
  DeliveryResult runJob(const Job& job) const;
  void collect(DeliveryResult r);
  void collectDone();
  void worker();
  void emitPending(double t_now, bool all);

  EngineConfig cfg_;
  Detector detector_;
  Tracker tracker_;
  LiveFollower follower_;
  int frame_index_ = 0;  // index of the next frame pushed
  std::vector<Candidate> last_candidates_;
  YuvFrame converted_;
  std::deque<std::pair<int, double>> detect_ms_per_frame_;
  std::deque<DeliveryResult> results_;
  std::vector<DeliveryResult> pending_;  // valid deliveries waiting for duplicates to close
  double t_now_ = 0.0;
  Stats stats_;

  // Background analysis. jobs_ and done_ are shared with the worker thread.
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Job> jobs_;
  std::deque<DeliveryResult> done_;
  std::vector<std::pair<double, double>> in_flight_;  // time spans of queued/running tracks
  int running_ = 0;
  bool stop_ = false;
  std::thread thread_;
};

}  // namespace pe
