#pragma once

#include <Eigen/Core>
#include <functional>

namespace pe {

// Small dense Levenberg-Marquardt solver with a numeric (central difference)
// Jacobian. Problems here have ~10 parameters and ~100 residuals.
struct LMOptions {
  int max_iterations = 100;
  double initial_lambda = 1e-3;
  double relative_tolerance = 1e-10;
  double step_scale = 1e-6;  // finite-difference step relative to max(1, |x|)
};

struct LMResult {
  Eigen::VectorXd x;
  Eigen::VectorXd residuals;
  Eigen::MatrixXd jacobian;  // evaluated at x
  double cost = 0.0;         // sum of squared residuals
  int iterations = 0;
  bool converged = false;
};

using ResidualFn = std::function<void(const Eigen::VectorXd& x, Eigen::VectorXd& residuals)>;

LMResult solveLM(const ResidualFn& fn, const Eigen::VectorXd& x0, const LMOptions& opt = {});

Eigen::MatrixXd numericJacobian(const ResidualFn& fn, const Eigen::VectorXd& x,
                                const Eigen::VectorXd& r0, double step_scale);

// Covariance (J^T J)^-1 scaled by max(1, cost / dof). Robust to near-singular J.
Eigen::MatrixXd covarianceFromJacobian(const Eigen::MatrixXd& J, double cost);

}  // namespace pe
