#include "sim/radar.h"

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "core/matrix.h"
#include "sim/constants.h"

namespace seashield::sim {
namespace {

RadarParams fast_scan_params() {
  RadarParams params;
  params.scan_period_s = 0.1;  // 6 ticks per scan: plenty of opportunities fast.
  return params;
}

// Runs the radar against a stationary target for whole scans, returning plots.
std::vector<Plot> run_scans(Radar& radar, const math::Vec3& target, int scans) {
  std::vector<Plot> plots;
  const auto ticks = static_cast<std::uint64_t>(scans) * radar.scan_ticks();
  for (std::uint64_t tick = 0; tick < ticks; ++tick) {
    radar.step(tick, {&target, 1}, plots);
  }
  return plots;
}

TEST(RadarTest, PdMonotonicallyDecreasesWithRange) {
  Radar radar(RadarParams{}, 0.0, 1);
  double previous = 1.1;
  for (double range = 1000.0; range <= 40000.0; range += 500.0) {
    const double pd = radar.detection_probability(range, 1000.0);
    EXPECT_LE(pd, previous) << "Pd increased at range " << range;
    previous = pd;
  }
  EXPECT_GT(radar.detection_probability(2000.0, 1000.0), 0.99);
  EXPECT_LT(radar.detection_probability(40000.0 * 4.0, 10000.0), 0.01);
}

TEST(RadarTest, PdIsHalfAtReferenceRangeInClearAir) {
  RadarParams params;
  params.reference_range_m = 25000.0;
  Radar radar(params, 0.0, 1);
  // High enough that the horizon does not interfere at 25 km.
  EXPECT_NEAR(radar.detection_probability(25000.0, 5000.0), 0.5, 1e-12);
}

TEST(RadarTest, RainAttenuationReducesPd) {
  RadarParams params;
  Radar clear(params, 0.0, 1);
  Radar raining(params, 1.0, 1);
  const double range = 20000.0;
  EXPECT_LT(raining.detection_probability(range, 5000.0),
            clear.detection_probability(range, 5000.0));
  // Attenuation grows with range: the gap widens further out.
  const double near_gap = clear.detection_probability(5000.0, 5000.0) -
                          raining.detection_probability(5000.0, 5000.0);
  const double far_gap = clear.detection_probability(25000.0, 5000.0) -
                         raining.detection_probability(25000.0, 5000.0);
  EXPECT_GT(far_gap, near_gap);
}

TEST(RadarTest, HorizonMasksLowAltitudeTargets) {
  Radar radar(RadarParams{}, 0.0, 1);
  // h_ant = 20 m, sea-skimmer at 10 m: R_h ≈ 18.4 + 13.0 ≈ 31.5 km.
  const double horizon = radar.horizon_range_m(10.0);
  EXPECT_NEAR(horizon, 31500.0, 600.0);
  EXPECT_EQ(radar.detection_probability(horizon + 1000.0, 10.0), 0.0);
  EXPECT_GT(radar.detection_probability(horizon - 1000.0, 10.0), 0.0);
  // The same range is NOT masked for a high flyer.
  EXPECT_GT(radar.detection_probability(horizon + 1000.0, 800.0), 0.0);
}

TEST(RadarTest, SeaSkimmerIsDetectedLaterThanHighFlyer) {
  // Both targets inbound from 38 km at 290 m/s; only altitude differs. The
  // skimmer must produce its first plot strictly later — the charter §5.4
  // mechanism that makes sea-skimming threats dangerous.
  const auto first_plot_tick = [](double altitude) -> std::uint64_t {
    RadarParams params = fast_scan_params();
    params.sigma_az_rad = 0.0;  // Keep geometry clean; detection is the subject.
    params.sigma_el_rad = 0.0;
    Radar radar(params, 0.0, 7);
    std::vector<Plot> plots;
    math::Vec3 position{0.0, 38000.0, altitude};
    const math::Vec3 step_south{0.0, -290.0 * kTickDt, 0.0};
    for (std::uint64_t tick = 0; tick < 6000; ++tick) {
      radar.step(tick, {&position, 1}, plots);
      if (!plots.empty()) {
        return tick;
      }
      position += step_south;
    }
    return 6000;
  };

  const std::uint64_t high_flyer = first_plot_tick(900.0);
  const std::uint64_t skimmer = first_plot_tick(10.0);
  EXPECT_LT(high_flyer, skimmer);
  EXPECT_LT(skimmer, 6000u) << "skimmer was never detected at all";
}

TEST(RadarTest, AtMostOneOpportunityPerScanAndPlotsCarryScanIndex) {
  RadarParams params = fast_scan_params();
  Radar radar(params, 0.0, 3);
  // 2 km target: Pd ≈ 1, so every opportunity becomes a plot.
  const auto plots = run_scans(radar, {1400.0, 1400.0, 300.0}, 50);
  ASSERT_EQ(plots.size(), 50u);
  for (std::size_t i = 0; i < plots.size(); ++i) {
    EXPECT_EQ(plots[i].scan_index, i) << "more or less than one plot per scan";
  }
}

TEST(RadarTest, MissedDetectionStillConsumesTheFixedDrawBudget) {
  // Target far beyond the horizon: Pd = 0, zero plots — but every crossing
  // must still consume 1 + 6 draws (the fixed-budget contract).
  RadarParams params = fast_scan_params();
  Radar radar(params, 0.0, 5);
  const std::uint64_t detection_before = radar.detection_rng_state();
  const std::uint64_t noise_before = radar.noise_rng_state();
  const auto plots = run_scans(radar, {0.0, 200000.0, 10.0}, 3);
  EXPECT_TRUE(plots.empty());
  EXPECT_NE(radar.detection_rng_state(), detection_before);
  EXPECT_NE(radar.noise_rng_state(), noise_before);

  // And the consumption is identical to a detectable target's: states after
  // N opportunities depend only on N, never on outcomes.
  Radar near_radar(params, 0.0, 5);
  std::vector<Plot> near_plots;
  const math::Vec3 near_target{1400.0, 1400.0, 300.0};
  for (std::uint64_t tick = 0; tick < 3 * near_radar.scan_ticks(); ++tick) {
    near_radar.step(tick, {&near_target, 1}, near_plots);
  }
  EXPECT_FALSE(near_plots.empty());
  EXPECT_EQ(near_radar.detection_rng_state(), radar.detection_rng_state());
  EXPECT_EQ(near_radar.noise_rng_state(), radar.noise_rng_state());
}

TEST(RadarTest, SameSeedReproducesIdenticalPlotSequence) {
  RadarParams params = fast_scan_params();
  Radar a(params, 0.3, 11);
  Radar b(params, 0.3, 11);
  const math::Vec3 target{9000.0, 12000.0, 400.0};
  const auto plots_a = run_scans(a, target, 40);
  const auto plots_b = run_scans(b, target, 40);
  ASSERT_EQ(plots_a.size(), plots_b.size());
  for (std::size_t i = 0; i < plots_a.size(); ++i) {
    EXPECT_EQ(plots_a[i].tick, plots_b[i].tick);
    EXPECT_EQ(plots_a[i].range_m, plots_b[i].range_m);  // Bit-identical.
    EXPECT_EQ(plots_a[i].azimuth_rad, plots_b[i].azimuth_rad);
    EXPECT_EQ(plots_a[i].elevation_rad, plots_b[i].elevation_rad);
  }
}

TEST(RadarTest, ZeroNoiseMeasurementRecoversTruthPosition) {
  RadarParams params = fast_scan_params();
  params.sigma_range_m = 0.0;
  params.sigma_az_rad = 0.0;
  params.sigma_el_rad = 0.0;
  Radar radar(params, 0.0, 1);
  const math::Vec3 truth{4000.0, 3000.0, 350.0};
  const auto plots = run_scans(radar, truth, 5);
  ASSERT_FALSE(plots.empty());
  EXPECT_NEAR(plots[0].position.x, truth.x, 1e-6);
  EXPECT_NEAR(plots[0].position.y, truth.y, 1e-6);
  EXPECT_NEAR(plots[0].position.z, truth.z, 1e-6);
}

TEST(RadarTest, NoiseStatisticsMatchConfiguredSigmas) {
  RadarParams params = fast_scan_params();
  Radar radar(params, 0.0, 42);
  const math::Vec3 truth{2000.0, 2000.0, 300.0};  // Close: Pd ≈ 1.
  const auto plots = run_scans(radar, truth, 4000);
  ASSERT_GT(plots.size(), 3000u);

  const double true_range = (truth - math::Vec3{0.0, 0.0, params.antenna_height_m}).norm();
  double sum = 0.0, sum_sq = 0.0;
  for (const Plot& plot : plots) {
    const double err = plot.range_m - true_range;
    sum += err;
    sum_sq += err * err;
  }
  const double n = static_cast<double>(plots.size());
  const double mean = sum / n;
  const double stddev = math::sqrt(sum_sq / n - mean * mean);
  // Seeded run → deterministic numbers; bounds are loose 3σ-of-the-estimator.
  EXPECT_NEAR(mean, 0.0, 3.0 * params.sigma_range_m / math::sqrt(n));
  EXPECT_NEAR(stddev, params.sigma_range_m, 0.05 * params.sigma_range_m);
}

TEST(RadarTest, ConvertedCovarianceIsSpdAndGrowsWithRange) {
  RadarParams params = fast_scan_params();
  params.sigma_range_m = 0.0;  // Isolate the angular contribution.
  Radar radar(params, 0.0, 9);
  const auto near_plots = run_scans(radar, {3000.0, 0.0, 300.0}, 3);
  Radar far_radar(params, 0.0, 9);
  const auto far_plots = run_scans(far_radar, {15000.0, 0.0, 5000.0}, 3);
  ASSERT_FALSE(near_plots.empty());
  ASSERT_FALSE(far_plots.empty());

  // SPD: the innovation-style inverse must exist.
  // (Rank: σ_r = 0 collapses one axis — add a floor like the tracker would.)
  const auto floored = [](math::Mat3 m) {
    for (int i = 0; i < 3; ++i) {
      m(i, i) += 1.0;
    }
    return m;
  };
  EXPECT_TRUE(math::inverse_spd(floored(near_plots[0].covariance)).has_value());

  // Cross-range variance scales with r²σ_az²: 5× range → 25× variance.
  const double near_cross = near_plots[0].covariance(1, 1);  // Target on +x axis.
  const double far_cross = far_plots[0].covariance(1, 1);
  EXPECT_GT(far_cross, near_cross * 10.0);
}

}  // namespace
}  // namespace seashield::sim
