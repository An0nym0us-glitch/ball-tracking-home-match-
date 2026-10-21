#include <gtest/gtest.h>

#include <cmath>
#include <random>

#include "pocketeye/lm.hpp"

using namespace pe;

TEST(LM, FitsExponential) {
  std::mt19937 rng(1);
  std::normal_distribution<double> nd(0, 0.01);
  std::vector<double> xs, ys;
  for (int i = 0; i < 40; ++i) {
    const double x = i * 0.1;
    xs.push_back(x);
    ys.push_back(2.5 * std::exp(-0.7 * x) + nd(rng));
  }
  const ResidualFn fn = [&](const Eigen::VectorXd& p, Eigen::VectorXd& r) {
    r.resize(xs.size());
    for (size_t i = 0; i < xs.size(); ++i) r[i] = (p[0] * std::exp(p[1] * xs[i]) - ys[i]) / 0.01;
  };
  Eigen::VectorXd x0(2);
  x0 << 1.0, 0.0;
  const LMResult res = solveLM(fn, x0);
  EXPECT_NEAR(res.x[0], 2.5, 0.02);
  EXPECT_NEAR(res.x[1], -0.7, 0.01);
  // Chi-square per degree of freedom ~ 1 for correctly weighted residuals.
  EXPECT_NEAR(res.cost / 38.0, 1.0, 0.5);
  const Eigen::MatrixXd cov = covarianceFromJacobian(res.jacobian, res.cost);
  EXPECT_GT(cov(0, 0), 0);
  EXPECT_LT(std::sqrt(cov(0, 0)), 0.02);
}

TEST(LM, Rosenbrock) {
  const ResidualFn fn = [](const Eigen::VectorXd& p, Eigen::VectorXd& r) {
    r.resize(2);
    r[0] = 10 * (p[1] - p[0] * p[0]);
    r[1] = 1 - p[0];
  };
  Eigen::VectorXd x0(2);
  x0 << -1.2, 1.0;
  LMOptions opt;
  opt.max_iterations = 500;
  const LMResult res = solveLM(fn, x0, opt);
  EXPECT_NEAR(res.x[0], 1.0, 1e-5);
  EXPECT_NEAR(res.x[1], 1.0, 1e-5);
}
