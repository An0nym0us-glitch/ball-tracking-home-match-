#pragma once

#include <deque>
#include <vector>

#include "pocketeye/detector.hpp"

namespace pe {

struct TrackPoint {
  int frame = 0;
  double t = 0.0;
  Vec2 uv = Vec2::Zero();
  double radius_px = 0.0;
  double strength = 0.0;
};

struct FrameCandidates {
  int frame = 0;
  double t = 0.0;
  std::vector<Candidate> cands;
  std::vector<bool> used;  // already part of an analysed path
};

// Ball path search: dynamic programming over a window of frames. Every
// detection is a node; a path is scored by the number of detections it
// explains minus penalties for missed frames and for unsmooth motion. Smooth
// motion means the next detection is where the previous two predict, with a
// tolerance that grows with the step (perspective makes the ball slow down in
// the image as it moves away). Sharp turns (bounce, pad) cost a fixed amount,
// so a ball path survives a few of them while chains of random fragments,
// which turn at every step, do not grow.
struct PathConfig {
  int max_gap = 3;              // frames between consecutive detections (2 missed)
  double max_step_frac = 0.2;   // max image step per frame, fraction of the image width
  double node_reward = 3.0;
  double gap_cost = 1.5;        // per missed frame
  double start_cost = 2.0;
  double kink_cost = 6.0;       // cap of the smoothness penalty
  double tol_px = 1.0;          // smoothness tolerance: tol_px + tol_rel * step + radius/4
  double tol_rel = 0.35;
  double min_step_px = 1.5;     // per frame: the ball always moves; flickering edges do not
  double strength_ref = 25.0;   // detections this strong get the full reward
  int min_points = 7;
  int max_paths = 6;            // per search
  int max_frame_candidates = 10;  // strongest per frame; bounds the search cost on noisy video
};

// Finds up to cfg.max_paths disjoint paths (best first) among unused candidates.
std::vector<std::vector<std::pair<int, int>>> findBallPaths(const std::deque<FrameCandidates>& frames,
                                                            const PathConfig& cfg, int image_width);

// Online wrapper: buffers candidates, searches periodically and hands out
// paths that have ended ("settled") for analysis.
class Tracker {
 public:
  Tracker(int image_width, PathConfig cfg = {}, double window_s = 2.6, double settle_s = 0.3);
  void update(int frame, double t, const std::vector<Candidate>& candidates);
  void flush();
  std::vector<std::vector<TrackPoint>> takeFinished(size_t min_points);
  size_t bufferedFrames() const { return frames_.size(); }

 private:
  void search(bool all);
  void stitch(const std::vector<TrackPoint>& track);
  int width_;
  PathConfig cfg_;
  double window_s_;
  double settle_s_;
  double t_now_ = 0.0;
  int since_search_ = 0;
  std::deque<FrameCandidates> frames_;
  std::vector<std::vector<TrackPoint>> finished_;
  std::deque<std::vector<TrackPoint>> recent_;  // handed out within the window, for stitch()
};

}  // namespace pe
