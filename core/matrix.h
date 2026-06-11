#pragma once

#include <cstddef>
#include <optional>

// Fixed-size dense matrices for the tracking filter (charter §5.5). Written
// by hand, like the rest of the math layer, because the zero-dependency rule
// (charter §8) is itself an appeal point — and because the Kalman filter in
// its standard form never needs a general inverse: the only inversion is the
// 3x3 innovation covariance, handled by the closed-form SPD routine below.
//
// Everything is double, row-major, value-semantic. Sizes are template
// parameters so dimension mismatches are compile errors, not runtime bugs.
namespace seashield::math {

template <int Rows, int Cols>
struct Mat {
  static_assert(Rows > 0 && Cols > 0);

  double m[Rows][Cols]{};

  constexpr double& operator()(int r, int c) { return m[r][c]; }
  constexpr double operator()(int r, int c) const { return m[r][c]; }

  // Column vectors read naturally with a single index.
  constexpr double& operator[](int i)
    requires(Cols == 1)
  {
    return m[i][0];
  }
  constexpr double operator[](int i) const
    requires(Cols == 1)
  {
    return m[i][0];
  }

  constexpr Mat operator+(const Mat& o) const {
    Mat out;
    for (int r = 0; r < Rows; ++r) {
      for (int c = 0; c < Cols; ++c) {
        out.m[r][c] = m[r][c] + o.m[r][c];
      }
    }
    return out;
  }
  constexpr Mat operator-(const Mat& o) const {
    Mat out;
    for (int r = 0; r < Rows; ++r) {
      for (int c = 0; c < Cols; ++c) {
        out.m[r][c] = m[r][c] - o.m[r][c];
      }
    }
    return out;
  }
  constexpr Mat operator*(double s) const {
    Mat out;
    for (int r = 0; r < Rows; ++r) {
      for (int c = 0; c < Cols; ++c) {
        out.m[r][c] = m[r][c] * s;
      }
    }
    return out;
  }

  constexpr Mat<Cols, Rows> transposed() const {
    Mat<Cols, Rows> out;
    for (int r = 0; r < Rows; ++r) {
      for (int c = 0; c < Cols; ++c) {
        out.m[c][r] = m[r][c];
      }
    }
    return out;
  }

  static constexpr Mat identity()
    requires(Rows == Cols)
  {
    Mat out;
    for (int i = 0; i < Rows; ++i) {
      out.m[i][i] = 1.0;
    }
    return out;
  }

  constexpr double trace() const
    requires(Rows == Cols)
  {
    double sum = 0.0;
    for (int i = 0; i < Rows; ++i) {
      sum += m[i][i];
    }
    return sum;
  }
};

template <int R, int K, int C>
constexpr Mat<R, C> operator*(const Mat<R, K>& a, const Mat<K, C>& b) {
  Mat<R, C> out;
  for (int r = 0; r < R; ++r) {
    for (int c = 0; c < C; ++c) {
      double sum = 0.0;
      for (int k = 0; k < K; ++k) {
        sum += a.m[r][k] * b.m[k][c];
      }
      out.m[r][c] = sum;
    }
  }
  return out;
}

using Mat3 = Mat<3, 3>;
using Mat6 = Mat<6, 6>;
using Vec6 = Mat<6, 1>;

// Closed-form (adjugate / determinant) inverse for the 3x3 innovation
// covariance. The input is symmetric positive definite by construction; a
// determinant at or below the guard means the covariance has collapsed to
// numerical garbage, and the caller must discard the plot rather than gate
// against a meaningless distance (tracking design, charter §5.5).
inline std::optional<Mat3> inverse_spd(const Mat3& s, double det_guard = 1e-12) {
  const double a = s(0, 0), b = s(0, 1), c = s(0, 2);
  const double d = s(1, 1), e = s(1, 2), f = s(2, 2);
  // Cofactors of the symmetric matrix [[a,b,c],[b,d,e],[c,e,f]].
  const double c00 = d * f - e * e;
  const double c01 = c * e - b * f;
  const double c02 = b * e - c * d;
  const double det = a * c00 + b * c01 + c * c02;
  if (!(det > det_guard)) {
    return std::nullopt;
  }
  const double inv_det = 1.0 / det;
  Mat3 out;
  out(0, 0) = c00 * inv_det;
  out(0, 1) = c01 * inv_det;
  out(0, 2) = c02 * inv_det;
  out(1, 0) = c01 * inv_det;
  out(1, 1) = (a * f - c * c) * inv_det;
  out(1, 2) = (b * c - a * e) * inv_det;
  out(2, 0) = c02 * inv_det;
  out(2, 1) = (b * c - a * e) * inv_det;
  out(2, 2) = (a * d - b * b) * inv_det;
  return out;
}

}  // namespace seashield::math
