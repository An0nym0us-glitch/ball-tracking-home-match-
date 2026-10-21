#include <gtest/gtest.h>

#include "pocketeye/geometry.hpp"

using namespace pe;

TEST(Geometry, LookAtOrientation) {
  const Camera cam = Camera::lookAt(1280, 720, 1000, Vec3(0, -2, 1.2), Vec3(0, 10, 0.3));
  const Vec2 centre = cam.project(Vec3(0, 10, 0.3));
  EXPECT_NEAR(centre.x(), 640, 1e-6);
  EXPECT_NEAR(centre.y(), 360, 1e-6);
  // +x (camera's right) appears right of centre, +z (up) appears above.
  EXPECT_GT(cam.project(Vec3(1, 10, 0.3)).x(), 640);
  EXPECT_LT(cam.project(Vec3(0, 10, 1.3)).y(), 360);
  EXPECT_NEAR((cam.center() - Vec3(0, -2, 1.2)).norm(), 0, 1e-9);
}

TEST(Geometry, RayRoundTrip) {
  const Camera cam = Camera::lookAt(1280, 720, 900, Vec3(0.2, -1.5, 1.3), Vec3(0, 12, 0.2), 0.03);
  const Vec3 p(0.4, 9.0, 0.55);
  const Vec2 uv = cam.project(p);
  const auto hit = intersectPlaneY(cam.center(), cam.rayDirection(uv), p.y());
  ASSERT_TRUE(hit.has_value());
  EXPECT_NEAR((*hit - p).norm(), 0, 1e-9);
  const auto g = intersectPlaneZ(cam.center(), cam.rayDirection(cam.project(Vec3(0.1, 7, 0))), 0.0);
  ASSERT_TRUE(g.has_value());
  EXPECT_NEAR((*g - Vec3(0.1, 7, 0)).norm(), 0, 1e-9);
}

TEST(Geometry, ApparentRadiusShrinksWithDistance) {
  const Camera cam = Camera::lookAt(1280, 720, 1000, Vec3(0, -2, 1.2), Vec3(0, 10, 0.3));
  const double near = cam.apparentRadius(Vec3(0, 3, 1.0), 0.0335);
  const double far = cam.apparentRadius(Vec3(0, 18, 0.2), 0.0335);
  EXPECT_GT(near, 3.5 * far);  // ~5 m vs ~20 m away
  EXPECT_NEAR(far, 1000 * 0.0335 / 20.0, 0.2);
}

TEST(Geometry, BoxIntersection) {
  const auto s = intersectBox(Vec3(0, -1, 1), Vec3(0, 1, 0), Vec3(-1, 0, 0), Vec3(1, 10, 2));
  ASSERT_TRUE(s.has_value());
  EXPECT_NEAR(s->first, 1.0, 1e-12);
  EXPECT_NEAR(s->second, 11.0, 1e-12);
  EXPECT_FALSE(intersectBox(Vec3(5, -1, 1), Vec3(0, 1, 0), Vec3(-1, 0, 0), Vec3(1, 10, 2)).has_value());
}
