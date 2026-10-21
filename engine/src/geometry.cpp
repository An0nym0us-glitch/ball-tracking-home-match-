#include "pocketeye/geometry.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace pe {

Vec2 Camera::project(const Vec3& world) const {
  const Vec3 c = toCamera(world);
  return {f * c.x() / c.z() + cx, f * c.y() / c.z() + cy};
}

Vec3 Camera::rayDirection(const Vec2& uv) const {
  const Vec3 d((uv.x() - cx) / f, (uv.y() - cy) / f, 1.0);
  return (R.transpose() * d).normalized();
}

double Camera::apparentRadius(const Vec3& world, double r) const {
  const Vec3 c = toCamera(world);
  const double dist = c.norm();
  if (c.z() <= 0.0 || dist <= r * 1.01) return 1e6;
  return f * r / std::sqrt(dist * dist - r * r);
}

Camera Camera::lookAt(int width, int height, double f, const Vec3& eye, const Vec3& target,
                      double roll_rad) {
  const Vec3 forward = (target - eye).normalized();
  Vec3 right = forward.cross(Vec3::UnitZ()).normalized();
  Vec3 down = forward.cross(right);
  if (roll_rad != 0.0) {
    const Eigen::AngleAxisd roll(roll_rad, forward);
    right = roll * right;
    down = roll * down;
  }
  Camera cam;
  cam.width = width;
  cam.height = height;
  cam.f = f;
  cam.cx = 0.5 * width;
  cam.cy = 0.5 * height;
  cam.R.row(0) = right.transpose();
  cam.R.row(1) = down.transpose();
  cam.R.row(2) = forward.transpose();
  cam.t = -cam.R * eye;
  return cam;
}

std::optional<Vec3> intersectPlaneZ(const Vec3& origin, const Vec3& dir, double z) {
  if (std::abs(dir.z()) < 1e-12) return std::nullopt;
  const double s = (z - origin.z()) / dir.z();
  if (s <= 0) return std::nullopt;
  return origin + s * dir;
}

std::optional<Vec3> intersectPlaneY(const Vec3& origin, const Vec3& dir, double y) {
  if (std::abs(dir.y()) < 1e-12) return std::nullopt;
  const double s = (y - origin.y()) / dir.y();
  if (s <= 0) return std::nullopt;
  return origin + s * dir;
}

std::optional<std::pair<double, double>> intersectBox(const Vec3& origin, const Vec3& dir,
                                                      const Vec3& lo, const Vec3& hi) {
  double s0 = 0.0;
  double s1 = std::numeric_limits<double>::infinity();
  for (int i = 0; i < 3; ++i) {
    if (std::abs(dir[i]) < 1e-12) {
      if (origin[i] < lo[i] || origin[i] > hi[i]) return std::nullopt;
      continue;
    }
    double a = (lo[i] - origin[i]) / dir[i];
    double b = (hi[i] - origin[i]) / dir[i];
    if (a > b) std::swap(a, b);
    s0 = std::max(s0, a);
    s1 = std::min(s1, b);
    if (s0 > s1) return std::nullopt;
  }
  return std::make_pair(s0, s1);
}

}  // namespace pe
