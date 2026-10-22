#pragma once

#include <deque>
#include <limits>
#include <vector>

#include "pocketeye/detector.hpp"
#include "pocketeye/geometry.hpp"

namespace pe {

// Where the ball is right now, for drawing on a live picture. Only for the
// screen: deliveries are found and measured by the tracker and the analysis.
struct LiveBall {
  bool visible = false;    // a moving ball is being followed
  bool predicted = false;  // not seen in the latest frame: position carried forward
  double t = 0.0;          // time of uv
  Vec2 uv = Vec2::Zero();  // px
  Vec2 velocity = Vec2::Zero();  // px/s
  double radius_px = 0.0;
  std::vector<Vec2> trail;  // recent positions, oldest first (ends at uv)
};

// Follows one moving ball through the detector's candidates frame by frame,
// like a camera following a face: predict where it goes, take the candidate
// closest to the prediction, carry it over a few missed frames. A ball is
// shown only after it moved consistently for a few frames, so flickering
// noise is not followed.
class LiveFollower {
 public:
  // target: the batter's stumps in the picture. A ball is only taken up while it
  // heads towards them (the bowler's arm swings up, back and round).
  explicit LiveFollower(int image_width, Vec2 target = Vec2::Constant(std::numeric_limits<double>::quiet_NaN()))
      : image_width_(image_width), target_(target) {}
  void update(double t, const std::vector<Candidate>& cands);
  // State predicted to t_now (at most a few frames ahead).
  LiveBall state(double t_now) const;

  static constexpr int kConfirmHits = 4;   // consistent detections before the ball is shown
  static constexpr int kMaxMisses = 4;     // frames carried forward without a detection
  static constexpr size_t kTrail = 45;     // trail length (points)

 private:
  struct Track {
    Vec2 p = Vec2::Zero();
    Vec2 v = Vec2::Zero();  // px/s; unknown (zero) after the first hit
    double r = 0.0;
    double t = 0.0;
    int hits = 0;
    int misses = 0;
    std::deque<Vec2> trail;
  };
  // Index of the candidate continuing `tr` at time t, or -1.
  int match(const Track& tr, double t, const std::vector<Candidate>& cands, const std::vector<bool>& used,
            bool confirmed) const;
  static void advance(Track& tr, double t, const Candidate& c);
  bool headsForTarget(const Track& tr) const;

  int image_width_;
  Vec2 target_;
  bool active_ = false;
  Track ball_;
  std::vector<Track> tentative_;
};

}  // namespace pe
