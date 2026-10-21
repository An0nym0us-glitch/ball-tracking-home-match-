#pragma once

#include <Eigen/Core>
#include <Eigen/Geometry>
#include <optional>
#include <utility>

namespace pe {

using Vec2 = Eigen::Vector2d;
using Vec3 = Eigen::Vector3d;
using Mat3 = Eigen::Matrix3d;

constexpr double kGravity = 9.81;  // m/s^2
constexpr double kPi = 3.14159265358979323846;

// World frame (metres): origin at the base of the bowler-end middle stump,
// +y along the pitch towards the batter's stumps, +x to the camera's right
// (camera looks down the pitch from behind the bowler), +z up.
// Camera frame follows OpenCV: x right, y down, z forward.
struct Camera {
  int width = 0;
  int height = 0;
  double f = 1000.0;  // focal length in pixels, square pixels
  double cx = 0.0;
  double cy = 0.0;
  Mat3 R = Mat3::Identity();  // world -> camera rotation
  Vec3 t = Vec3::Zero();      // world -> camera translation

  Vec3 toCamera(const Vec3& world) const { return R * world + t; }
  Vec3 center() const { return -R.transpose() * t; }
  double depth(const Vec3& world) const { return toCamera(world).z(); }
  Vec2 project(const Vec3& world) const;
  // Unit direction (world frame) of the ray through pixel uv.
  Vec3 rayDirection(const Vec2& uv) const;
  // Projected radius in pixels of a sphere of radius r centred at `world`.
  double apparentRadius(const Vec3& world, double r) const;

  static Camera lookAt(int width, int height, double f, const Vec3& eye, const Vec3& target,
                       double roll_rad = 0.0);
};

// Pitch and stump dimensions. Home pitches vary, so nothing is fixed to 22 yards.
struct Pitch {
  double length = 20.12;          // bowler stumps line to batter stumps line (m)
  double width = 3.05;            // width between tapped pitch corners, if used (m)
  double stump_height = 0.711;    // ground to top of the stumps/bails (m)
  double stump_width = 0.2286;    // outer edge of left stump to outer edge of right stump (m)
  double stump_diameter = 0.035;  // single stump (m)

  // x of the centre line of the outer stumps.
  double outerStumpX() const { return 0.5 * (stump_width - stump_diameter); }
};

std::optional<Vec3> intersectPlaneZ(const Vec3& origin, const Vec3& dir, double z);
std::optional<Vec3> intersectPlaneY(const Vec3& origin, const Vec3& dir, double y);
// Slab test against an axis-aligned box. Returns the [s_min, s_max] ray parameters (s >= 0).
std::optional<std::pair<double, double>> intersectBox(const Vec3& origin, const Vec3& dir,
                                                      const Vec3& lo, const Vec3& hi);

}  // namespace pe
