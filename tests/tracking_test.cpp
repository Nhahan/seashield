#include "sim/tracking.h"

#include <cmath>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/pcg32.h"
#include "sim/constants.h"

namespace seashield::sim {
namespace {

math::Mat3 diagonal3(double a, double b, double c) {
  math::Mat3 m;
  m(0, 0) = a;
  m(1, 1) = b;
  m(2, 2) = c;
  return m;
}

Plot make_plot(const math::Vec3& position, const math::Mat3& covariance, std::uint64_t tick = 0,
               std::uint32_t scan = 0) {
  Plot plot;
  plot.tick = tick;
  plot.scan_index = scan;
  plot.position = position;
  plot.covariance = covariance;
  return plot;
}

// Mirrors kalman_ref.py: deterministic pseudo-noise instead of a PRNG, so the
// reference comparison tests filter algebra, not noise-source parity.
math::Vec3 pseudo_noise(int k) {
  return {9.0 * math::sin(0.7 * k + 0.3), 7.0 * math::sin(1.1 * k + 1.2),
          5.0 * math::sin(1.9 * k + 2.1)};
}

// --- KalmanFilter: reference and consistency ---------------------------------

TEST(KalmanFilterTest, MatchesIndependentNumpyReference) {
  std::ifstream golden(std::string(SEASHIELD_SOURCE_DIR) + "/tests/golden/kalman_ref.csv");
  ASSERT_TRUE(golden.is_open()) << "run tools/reference/kalman_ref.py first";

  struct Sample {
    int step;
    double x[6];
    double p_lower[21];
  };
  std::vector<Sample> samples;
  std::string line;
  std::getline(golden, line);  // Header.
  while (std::getline(golden, line)) {
    std::istringstream ss(line);
    std::string field;
    Sample sample{};
    std::getline(ss, field, ',');
    sample.step = std::stoi(field);
    for (double& v : sample.x) {
      std::getline(ss, field, ',');
      v = std::stod(field);
    }
    for (double& v : sample.p_lower) {
      std::getline(ss, field, ',');
      v = std::stod(field);
    }
    samples.push_back(sample);
  }
  ASSERT_EQ(samples.size(), 5u);

  // Same configuration as the script.
  KalmanFilter filter(1.0 / 60.0, 30.0);
  math::Vec6 x0;
  const double x0_values[6] = {8050.0, 5970.0, 320.0, 0.0, 0.0, 0.0};
  for (int i = 0; i < 6; ++i) {
    x0[i] = x0_values[i];
  }
  math::Mat6 p0;
  const double p0_diag[6] = {900.0, 900.0, 400.0, 160000.0, 160000.0, 160000.0};
  for (int i = 0; i < 6; ++i) {
    p0(i, i) = p0_diag[i];
  }
  filter.set_state(x0, p0);
  const math::Mat3 r = diagonal3(100.0, 64.0, 81.0);

  math::Vec6 truth;
  const double truth0[6] = {8000.0, 6000.0, 300.0, -180.0, -120.0, 0.0};
  for (int i = 0; i < 6; ++i) {
    truth[i] = truth0[i];
  }

  std::size_t next_sample = 0;
  for (int step = 1; step <= 60; ++step) {
    for (int axis = 0; axis < 3; ++axis) {
      truth[axis] += truth[axis + 3] * (1.0 / 60.0);
    }
    filter.predict();
    const math::Vec3 noise = pseudo_noise(step);
    const math::Vec3 z{truth[0] + noise.x, truth[1] + noise.y, truth[2] + noise.z};
    ASSERT_TRUE(filter.update(z, r));

    if (next_sample < samples.size() && samples[next_sample].step == step) {
      const Sample& expected = samples[next_sample];
      for (int i = 0; i < 6; ++i) {
        EXPECT_NEAR(filter.state()[i], expected.x[i],
                    std::abs(expected.x[i]) * 1e-9 + 1e-9)
            << "x[" << i << "] at step " << step;
      }
      int flat = 0;
      for (int row = 0; row < 6; ++row) {
        for (int col = 0; col <= row; ++col) {
          EXPECT_NEAR(filter.covariance()(row, col), expected.p_lower[flat],
                      std::abs(expected.p_lower[flat]) * 1e-9 + 1e-9)
              << "P(" << row << "," << col << ") at step " << step;
          ++flat;
        }
      }
      ++next_sample;
    }
  }
  EXPECT_EQ(next_sample, samples.size());
}

TEST(KalmanFilterTest, CovarianceStaysSymmetricPositiveOnLongRuns) {
  KalmanFilter filter(kTickDt, 30.0);
  math::Vec6 x0;
  math::Mat6 p0 = math::Mat6::identity() * 1000.0;
  filter.set_state(x0, p0);
  const math::Mat3 r = diagonal3(900.0, 900.0, 900.0);

  Pcg32 rng(99);
  for (int step = 0; step < 5000; ++step) {
    filter.predict();
    if (step % 10 == 0) {
      const math::Vec3 z{rng.gaussian(0.0, 30.0), rng.gaussian(0.0, 30.0),
                         rng.gaussian(0.0, 30.0)};
      ASSERT_TRUE(filter.update(z, r));
    }
  }
  const math::Mat6& p = filter.covariance();
  for (int row = 0; row < 6; ++row) {
    EXPECT_GT(p(row, row), 0.0);
    for (int col = 0; col < 6; ++col) {
      EXPECT_NEAR(p(row, col), p(col, row), 1e-9);
    }
  }
}

TEST(KalmanFilterTest, ConvergesOnConstantVelocityTarget) {
  KalmanFilter filter(kTickDt, 5.0);
  math::Vec6 x0;
  x0[0] = 10000.0;
  x0[1] = 10000.0;
  x0[2] = 400.0;
  math::Mat6 p0 = math::Mat6::identity() * 1.0e6;
  filter.set_state(x0, p0);

  const math::Vec3 velocity{-200.0, -150.0, 0.0};
  math::Vec3 truth{10000.0, 10000.0, 400.0};
  const double sigma = 25.0;
  const math::Mat3 r = diagonal3(sigma * sigma, sigma * sigma, sigma * sigma);
  Pcg32 rng(7);

  double final_error = 1e30;
  for (int step = 0; step < 1200; ++step) {
    truth += velocity * kTickDt;
    filter.predict();
    if (step % 12 == 0) {  // Plots at scan rate, not tick rate.
      const math::Vec3 z{truth.x + rng.gaussian(0.0, sigma), truth.y + rng.gaussian(0.0, sigma),
                         truth.z + rng.gaussian(0.0, sigma)};
      ASSERT_TRUE(filter.update(z, r));
    }
    final_error = (filter.position() - truth).norm();
  }
  // After 100 updates the estimate must be well inside one measurement sigma,
  // and the velocity must have been inferred from positions alone.
  EXPECT_LT(final_error, sigma);
  EXPECT_LT((filter.velocity() - velocity).norm(), 20.0);
}

// Time-averaged consistency statistics (charter §5.5 "모델과 필터의 정합성").
// The dangerous failure mode is OVERCONFIDENCE (NEES/NIS far above the dof):
// a filter that trusts a wrong state gets targets killed. The conservative
// direction (below dof) merely wastes information, so the lower bounds are
// loose. Bounds derive from the chi-square mean (3 dof) with generous margin;
// the seeded run makes the numbers deterministic. Derivation:
// docs/architecture/simulation-models.md §6.
TEST(KalmanFilterTest, NeesAndNisAreChiSquareConsistentOnMatchedModel) {
  const double sigma = 30.0;
  KalmanFilter filter(kTickDt, 2.0);
  math::Vec6 x0;
  x0[0] = 8000.0;
  x0[1] = 8000.0;
  x0[2] = 300.0;
  math::Mat6 p0 = math::Mat6::identity() * 1.0e4;
  filter.set_state(x0, p0);

  math::Vec3 truth{8000.0, 8000.0, 300.0};
  const math::Vec3 velocity{-220.0, -140.0, 0.0};
  const math::Mat3 r = diagonal3(sigma * sigma, sigma * sigma, sigma * sigma);
  Pcg32 rng(31);

  double nees_sum = 0.0;
  double nis_sum = 0.0;
  int count = 0;
  for (int step = 0; step < 4800; ++step) {
    truth += velocity * kTickDt;
    filter.predict();
    if (step % 12 != 0) {
      continue;
    }
    const math::Vec3 z{truth.x + rng.gaussian(0.0, sigma), truth.y + rng.gaussian(0.0, sigma),
                       truth.z + rng.gaussian(0.0, sigma)};
    const auto nis = filter.gate_distance_sq(z, r);
    ASSERT_TRUE(nis.has_value());
    ASSERT_TRUE(filter.update(z, r));

    if (step < 1200) {
      continue;  // Skip the initial transient.
    }
    // Position NEES via the position block of P (3 dof) — avoids a 6x6
    // inverse the filter itself never needs.
    math::Mat3 p_pos;
    for (int row = 0; row < 3; ++row) {
      for (int col = 0; col < 3; ++col) {
        p_pos(row, col) = filter.covariance()(row, col);
      }
    }
    const auto p_inv = math::inverse_spd(p_pos);
    ASSERT_TRUE(p_inv.has_value());
    const math::Vec3 err = filter.position() - truth;
    double nees = 0.0;
    const double err_v[3] = {err.x, err.y, err.z};
    for (int row = 0; row < 3; ++row) {
      for (int col = 0; col < 3; ++col) {
        nees += err_v[row] * (*p_inv)(row, col) * err_v[col];
      }
    }
    nees_sum += nees;
    nis_sum += *nis;
    ++count;
  }
  ASSERT_GT(count, 200);
  const double mean_nees = nees_sum / count;
  const double mean_nis = nis_sum / count;
  EXPECT_GT(mean_nees, 0.5);
  EXPECT_LT(mean_nees, 3.8);
  EXPECT_GT(mean_nis, 0.5);
  EXPECT_LT(mean_nis, 3.6);
}

TEST(KalmanFilterTest, OverconfidentFilterFailsTheNeesCheck) {
  // Same scenario but the target weaves while the filter's Q claims it never
  // maneuvers: the NEES must blow through the consistency ceiling — proof the
  // statistic actually detects overconfidence (and the experiment-report
  // basis for "CV 필터는 기동에 과신으로 반응한다").
  const double sigma = 30.0;
  KalmanFilter filter(kTickDt, 0.05);  // Q ~ none: "targets never accelerate".
  math::Vec6 x0;
  x0[0] = 8000.0;
  x0[1] = 8000.0;
  x0[2] = 300.0;
  math::Mat6 p0 = math::Mat6::identity() * 1.0e4;
  filter.set_state(x0, p0);

  math::Vec3 truth{8000.0, 8000.0, 300.0};
  math::Vec3 velocity{-220.0, -140.0, 0.0};
  const math::Mat3 r = diagonal3(sigma * sigma, sigma * sigma, sigma * sigma);
  Pcg32 rng(31);

  double nees_sum = 0.0;
  int count = 0;
  for (int step = 0; step < 4800; ++step) {
    // Weaving: sinusoidal lateral acceleration of ~3g.
    const double accel = 30.0 * math::sin(2.0 * math::kPi * step * kTickDt / 4.0);
    velocity.x += accel * kTickDt;
    truth += velocity * kTickDt;
    filter.predict();
    if (step % 12 != 0) {
      continue;
    }
    const math::Vec3 z{truth.x + rng.gaussian(0.0, sigma), truth.y + rng.gaussian(0.0, sigma),
                       truth.z + rng.gaussian(0.0, sigma)};
    ASSERT_TRUE(filter.update(z, r));
    if (step < 1200) {
      continue;
    }
    math::Mat3 p_pos;
    for (int row = 0; row < 3; ++row) {
      for (int col = 0; col < 3; ++col) {
        p_pos(row, col) = filter.covariance()(row, col);
      }
    }
    const auto p_inv = math::inverse_spd(p_pos);
    ASSERT_TRUE(p_inv.has_value());
    const math::Vec3 err = filter.position() - truth;
    const double err_v[3] = {err.x, err.y, err.z};
    for (int row = 0; row < 3; ++row) {
      for (int col = 0; col < 3; ++col) {
        nees_sum += err_v[row] * (*p_inv)(row, col) * err_v[col];
      }
    }
    ++count;
  }
  EXPECT_GT(nees_sum / count, 3.8) << "overconfidence went undetected";
}

TEST(KalmanFilterTest, ManeuverInducesLagBeyondStraightBaseline) {
  // Identical noise seeds; the only difference is a 6°/s turn. CV tracking
  // error during the turn must exceed the straight-line baseline by a clear
  // factor — the unit-level evidence behind the §2.4 experiment narrative.
  const auto mean_error = [](bool turning) {
    const double sigma = 30.0;
    KalmanFilter filter(kTickDt, 10.0);
    math::Vec6 x0;
    x0[0] = 12000.0;
    x0[1] = 0.0;
    x0[2] = 300.0;
    math::Mat6 p0 = math::Mat6::identity() * 1.0e4;
    filter.set_state(x0, p0);

    math::Vec3 truth{12000.0, 0.0, 300.0};
    double heading = math::deg_to_rad(270.0);  // Inbound along -x.
    const double speed = 250.0;
    const double turn_rate = turning ? math::deg_to_rad(6.0) : 0.0;
    const math::Mat3 r = diagonal3(sigma * sigma, sigma * sigma, sigma * sigma);
    Pcg32 rng(17);

    double error_sum = 0.0;
    int samples = 0;
    for (int step = 0; step < 2400; ++step) {
      heading += turn_rate * kTickDt;
      truth.x += speed * math::sin(heading) * kTickDt;
      truth.y += speed * math::cos(heading) * kTickDt;
      filter.predict();
      if (step % 12 == 0) {
        const math::Vec3 z{truth.x + rng.gaussian(0.0, sigma),
                           truth.y + rng.gaussian(0.0, sigma),
                           truth.z + rng.gaussian(0.0, sigma)};
        filter.update(z, r);
      }
      if (step >= 1200) {  // Measure once the filter has settled.
        error_sum += (filter.position() - truth).norm();
        ++samples;
      }
    }
    return error_sum / samples;
  };

  const double straight = mean_error(false);
  const double turning = mean_error(true);
  EXPECT_GT(turning, straight * 1.5);
}

TEST(KalmanFilterTest, CoastingGrowsCovarianceMonotonically) {
  KalmanFilter filter(kTickDt, 30.0);
  math::Vec6 x0;
  math::Mat6 p0 = math::Mat6::identity() * 100.0;
  filter.set_state(x0, p0);
  double previous_trace = filter.covariance().trace();
  for (int step = 0; step < 300; ++step) {
    filter.predict();
    const double trace = filter.covariance().trace();
    EXPECT_GT(trace, previous_trace);
    previous_trace = trace;
  }
}

TEST(KalmanFilterTest, SingularInnovationIsRejectedWithoutTouchingState) {
  KalmanFilter filter(kTickDt, 30.0);
  math::Vec6 x0;
  x0[0] = 5.0;
  filter.set_state(x0, math::Mat6{});  // P = 0 and R = 0 → S = 0.
  EXPECT_FALSE(filter.gate_distance_sq({1.0, 2.0, 3.0}, math::Mat3{}).has_value());
  EXPECT_FALSE(filter.update({1.0, 2.0, 3.0}, math::Mat3{}));
  EXPECT_DOUBLE_EQ(filter.state()[0], 5.0);
}

// --- Tracker: lifecycle management --------------------------------------------

class TrackerLifecycleTest : public ::testing::Test {
 protected:
  TrackerLifecycleTest() : tracker_(TrackerParams{}) {}

  // One radar scan in miniature: optionally one plot, then the boundary.
  void scan(bool with_plot, const math::Vec3& position = {5000.0, 5000.0, 300.0}) {
    if (with_plot) {
      const Plot plot = make_plot(position, diagonal3(900.0, 900.0, 900.0), tick_);
      tracker_.update({&plot, 1}, tick_);
    }
    tracker_.on_scan_boundary(tick_);
    tick_ += 120;  // 2 s scan period in ticks; exact value irrelevant here.
  }

  Tracker tracker_;
  std::uint64_t tick_ = 0;
};

TEST_F(TrackerLifecycleTest, TrackConfirmsExactlyAtMOfN) {
  scan(true);  // Hit 1: initiated, tentative.
  ASSERT_EQ(tracker_.tracks().size(), 1u);
  EXPECT_EQ(tracker_.tracks()[0].status, TrackStatus::kTentative);
  scan(true);  // Hit 2: still tentative (M = 3).
  EXPECT_EQ(tracker_.tracks()[0].status, TrackStatus::kTentative);
  scan(true);  // Hit 3: confirmed exactly now.
  EXPECT_EQ(tracker_.tracks()[0].status, TrackStatus::kConfirmed);

  const auto events = tracker_.drain_events();
  ASSERT_EQ(events.size(), 2u);
  EXPECT_EQ(events[0].kind, TrackEvent::Kind::kInitiated);
  EXPECT_EQ(events[1].kind, TrackEvent::Kind::kConfirmed);
}

TEST_F(TrackerLifecycleTest, TentativeTrackWithInsufficientHitsDrops) {
  scan(true);
  scan(true);  // Two hits — one short of confirmation.
  scan(false);
  scan(false);
  scan(false);  // Three consecutive missed scans.
  EXPECT_TRUE(tracker_.tracks().empty());
  const auto events = tracker_.drain_events();
  ASSERT_EQ(events.size(), 2u);
  EXPECT_EQ(events[1].kind, TrackEvent::Kind::kDropped);
}

TEST_F(TrackerLifecycleTest, ConfirmedTrackDropsAfterExactMissStreak) {
  scan(true);
  scan(true);
  scan(true);  // Confirmed.
  scan(false);
  scan(false);
  ASSERT_EQ(tracker_.tracks().size(), 1u);  // Two misses: still coasting.
  EXPECT_TRUE(tracker_.tracks()[0].coasting());
  scan(false);  // Third consecutive miss: dropped.
  EXPECT_TRUE(tracker_.tracks().empty());
}

TEST_F(TrackerLifecycleTest, ReacquisitionResetsTheMissStreak) {
  scan(true);
  scan(true);
  scan(true);
  scan(false);
  scan(false);
  scan(true, {4000.0, 4100.0, 300.0});  // Reacquired near the coasted estimate...
  ASSERT_EQ(tracker_.tracks().size(), 1u);
  EXPECT_FALSE(tracker_.tracks()[0].coasting());
  scan(false);
  scan(false);
  EXPECT_EQ(tracker_.tracks().size(), 1u);  // Streak restarted from zero.
}

TEST(TrackerTest, GateRejectsFarPlotsAndSpawnsNewTrack) {
  Tracker tracker((TrackerParams{}));
  const math::Mat3 r = diagonal3(900.0, 900.0, 900.0);
  const Plot first = make_plot({5000.0, 5000.0, 300.0}, r, 0);
  tracker.update({&first, 1}, 0);
  ASSERT_EQ(tracker.tracks().size(), 1u);

  // 10 km away: hopelessly outside any gate → second track, not an update.
  const Plot far = make_plot({15000.0, 5000.0, 300.0}, r, 1);
  tracker.update({&far, 1}, 1);
  ASSERT_EQ(tracker.tracks().size(), 2u);
  EXPECT_EQ(tracker.tracks()[0].id, 1u);
  EXPECT_EQ(tracker.tracks()[1].id, 2u);
}

TEST(TrackerTest, NearestNeighbourAssociatesTheCloserTrack) {
  Tracker tracker((TrackerParams{}));
  const math::Mat3 r = diagonal3(900.0, 900.0, 900.0);
  const Plot a = make_plot({5000.0, 5000.0, 300.0}, r, 0);
  const Plot b = make_plot({5400.0, 5000.0, 300.0}, r, 0);
  tracker.update({&a, 1}, 0);
  tracker.update({&b, 1}, 0);
  ASSERT_EQ(tracker.tracks().size(), 2u);

  // Plot near track 2: only track 2's estimate may move.
  const math::Vec3 before_1 = tracker.tracks()[0].position();
  const Plot near_b = make_plot({5390.0, 5010.0, 300.0}, r, 1);
  tracker.update({&near_b, 1}, 1);
  ASSERT_EQ(tracker.tracks().size(), 2u);
  EXPECT_EQ((tracker.tracks()[0].position() - before_1).norm(), 0.0);
  EXPECT_GT(tracker.tracks()[1].last_update_tick, 0u);
}

TEST(TrackerTest, TrackIdsAreNeverRecycled) {
  Tracker tracker((TrackerParams{}));
  const math::Mat3 r = diagonal3(900.0, 900.0, 900.0);
  const Plot plot = make_plot({5000.0, 5000.0, 300.0}, r, 0);
  tracker.update({&plot, 1}, 0);
  // Kill it via miss streak.
  for (int scan = 0; scan < 4; ++scan) {
    tracker.on_scan_boundary(static_cast<std::uint64_t>(scan));
  }
  ASSERT_TRUE(tracker.tracks().empty());

  const Plot reborn = make_plot({5000.0, 5000.0, 300.0}, r, 500);
  tracker.update({&reborn, 1}, 500);
  ASSERT_EQ(tracker.tracks().size(), 1u);
  EXPECT_EQ(tracker.tracks()[0].id, 2u) << "track ids must not be recycled";
}

TEST(TrackerTest, IdenticalPlotSequencesProduceIdenticalTracks) {
  const auto run = [] {
    Tracker tracker((TrackerParams{}));
    Pcg32 rng(5);
    std::uint64_t tick = 0;
    for (int scan = 0; scan < 20; ++scan) {
      // The world predicts every tick between scans (120 ticks at a 2 s scan
      // period) — that coast is what inflates the gate enough to follow a
      // moving target whose initial track has zero velocity.
      for (int i = 0; i < 120; ++i) {
        tracker.predict();
      }
      const math::Vec3 position{8000.0 - 300.0 * scan + rng.gaussian(0.0, 25.0),
                                8000.0 - 200.0 * scan + rng.gaussian(0.0, 25.0),
                                300.0 + rng.gaussian(0.0, 25.0)};
      const Plot plot = make_plot(position, diagonal3(625.0, 625.0, 625.0), tick);
      tracker.update({&plot, 1}, tick);
      tracker.on_scan_boundary(tick);
      tick += 120;
    }
    return tracker;
  };
  const Tracker a = run();
  const Tracker b = run();
  ASSERT_EQ(a.tracks().size(), b.tracks().size());
  ASSERT_EQ(a.tracks().size(), 1u);
  for (int i = 0; i < 6; ++i) {
    EXPECT_EQ(a.tracks()[0].filter.state()[i], b.tracks()[0].filter.state()[i]);  // Bit-equal.
  }
  EXPECT_EQ(a.next_track_id(), b.next_track_id());
}

}  // namespace
}  // namespace seashield::sim
