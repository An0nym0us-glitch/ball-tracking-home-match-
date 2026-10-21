#include "pocketeye/lm.hpp"

#include <Eigen/Dense>
#include <algorithm>
#include <cmath>

namespace pe {

Eigen::MatrixXd numericJacobian(const ResidualFn& fn, const Eigen::VectorXd& x,
                                const Eigen::VectorXd& r0, double step_scale) {
  Eigen::MatrixXd J(r0.size(), x.size());
  Eigen::VectorXd xp = x;
  Eigen::VectorXd rp, rm;
  for (Eigen::Index j = 0; j < x.size(); ++j) {
    const double h = step_scale * std::max(1.0, std::abs(x[j]));
    xp[j] = x[j] + h;
    fn(xp, rp);
    xp[j] = x[j] - h;
    fn(xp, rm);
    xp[j] = x[j];
    J.col(j) = (rp - rm) / (2.0 * h);
  }
  return J;
}

LMResult solveLM(const ResidualFn& fn, const Eigen::VectorXd& x0, const LMOptions& opt) {
  LMResult res;
  res.x = x0;
  fn(res.x, res.residuals);
  res.cost = res.residuals.squaredNorm();
  double lambda = opt.initial_lambda;
  const Eigen::Index n = x0.size();

  for (int it = 0; it < opt.max_iterations; ++it) {
    res.iterations = it + 1;
    const Eigen::MatrixXd J = numericJacobian(fn, res.x, res.residuals, opt.step_scale);
    const Eigen::MatrixXd JtJ = J.transpose() * J;
    const Eigen::VectorXd g = J.transpose() * res.residuals;

    bool improved = false;
    for (int attempt = 0; attempt < 12; ++attempt) {
      Eigen::MatrixXd A = JtJ;
      for (Eigen::Index i = 0; i < n; ++i) A(i, i) += lambda * std::max(JtJ(i, i), 1e-9);
      const Eigen::VectorXd dx = A.ldlt().solve(-g);
      if (!dx.allFinite()) {
        lambda *= 10.0;
        continue;
      }
      const Eigen::VectorXd xn = res.x + dx;
      Eigen::VectorXd rn;
      fn(xn, rn);
      const double cn = rn.squaredNorm();
      if (std::isfinite(cn) && cn < res.cost) {
        const double rel = (res.cost - cn) / std::max(res.cost, 1e-300);
        res.x = xn;
        res.residuals = rn;
        res.cost = cn;
        lambda = std::max(lambda * 0.3, 1e-12);
        improved = true;
        if (rel < opt.relative_tolerance || dx.norm() < 1e-12 * (1.0 + res.x.norm())) {
          res.converged = true;
        }
        break;
      }
      lambda *= 10.0;
    }
    if (!improved) {
      res.converged = true;  // no descent direction left: local minimum
      break;
    }
    if (res.converged) break;
  }
  res.jacobian = numericJacobian(fn, res.x, res.residuals, opt.step_scale);
  return res;
}

Eigen::MatrixXd covarianceFromJacobian(const Eigen::MatrixXd& J, double cost) {
  const Eigen::Index m = J.rows();
  const Eigen::Index n = J.cols();
  const double dof = std::max<double>(1.0, static_cast<double>(m - n));
  const double s2 = std::max(1.0, cost / dof);
  const Eigen::MatrixXd JtJ = J.transpose() * J;
  Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(JtJ);
  Eigen::VectorXd inv = es.eigenvalues();
  const double maxev = std::max(inv.maxCoeff(), 1e-30);
  for (Eigen::Index i = 0; i < n; ++i) {
    inv[i] = inv[i] > maxev * 1e-14 ? 1.0 / inv[i] : 0.0;
  }
  return s2 * es.eigenvectors() * inv.asDiagonal() * es.eigenvectors().transpose();
}

}  // namespace pe
