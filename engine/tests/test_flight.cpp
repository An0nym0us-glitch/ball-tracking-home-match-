#include <gtest/gtest.h>

#include "pocketeye/flight.hpp"

using namespace pe;

TEST(Flight, NoDragMatchesBallistics) {
  const Vec3 p0(0.1, 1.0, 2.0), v0(0.3, 25.0, -2.0);
  const FlightPath fp(p0, v0, 0.5, 0.0, 1.5, FlightParams{0.0, 0.0});
  for (double t : {0.0, 0.37, 0.5, 0.81, 1.5}) {
    const double s = t - 0.5;
    const Vec3 expect = p0 + v0 * s + 0.5 * Vec3(0, 0, -kGravity) * s * s;
    EXPECT_NEAR((fp.position(t) - expect).norm(), 0.0, 1e-9) << t;
    EXPECT_NEAR((fp.velocity(t) - (v0 + Vec3(0, 0, -kGravity) * s)).norm(), 0.0, 1e-7) << t;
  }
}

TEST(Flight, LateralAccelerationGivesSwing) {
  const FlightPath fp(Vec3(0, 0, 2), Vec3(0, 25, 0), 0.0, 0.0, 0.6, FlightParams{0.0, 2.0});
  // 0.5 * a * t^2 = 0.36 m after 0.6 s.
  EXPECT_NEAR(fp.position(0.6).x(), 0.36, 1e-9);
}

TEST(Flight, DragSlowsTheBall) {
  const FlightPath fp(Vec3(0, 0, 2), Vec3(0, 25, 0), 0.0, 0.0, 0.6, FlightParams{0.02, 0.0});
  const double v_end = fp.velocity(0.6).norm();
  EXPECT_LT(v_end, 25.0);
  // dv/dt = -k v^2  ->  v(t) = v0 / (1 + k v0 t) (horizontal part dominates).
  EXPECT_NEAR(fp.velocity(0.6).y(), 25.0 / (1 + 0.02 * 25 * 0.6), 0.15);
}

TEST(Flight, BackwardAndForwardAgree) {
  const FlightParams fp{0.015, 1.0};
  const FlightPath a(Vec3(0.1, 2.0, 1.8), Vec3(0.2, 24, -1.0), 0.0, -0.2, 0.6, fp);
  const Vec3 p = a.position(0.4), v = a.velocity(0.4);
  const FlightPath b(p, v, 0.4, -0.2, 0.6, fp);
  EXPECT_NEAR((b.position(0.0) - Vec3(0.1, 2.0, 1.8)).norm(), 0.0, 1e-6);
  EXPECT_NEAR((b.position(-0.1) - a.position(-0.1)).norm(), 0.0, 1e-6);
}

TEST(Flight, TimeAtY) {
  const FlightPath fp(Vec3(0, 0, 2), Vec3(0, 20, 0), 0.0, 0.0, 1.0, FlightParams{0.0, 0.0});
  const auto t = fp.timeAtY(15.0, 0.0, 1.0);
  ASSERT_TRUE(t.has_value());
  EXPECT_NEAR(*t, 0.75, 1e-9);
  EXPECT_FALSE(fp.timeAtY(30.0, 0.0, 1.0).has_value());
}
