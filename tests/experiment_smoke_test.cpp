#include "tools/experiment/experiment.h"

#include <gtest/gtest.h>

#include "sim/scenario.h"

// CI smoke for the experiment harness (the full sweeps run offline — charter
// §10.3 keeps slow loads out of the test gate): the grid plumbing must parse,
// apply, run, and emit reproducible rows.
namespace seashield::experiment {
namespace {

TEST(ExperimentSmokeTest, AxisParsingHandlesRangesListsAndGarbage) {
  const auto range = parse_axis("range=3000:5000:1000");
  ASSERT_TRUE(range.has_value());
  EXPECT_EQ(range->name, "range");
  ASSERT_EQ(range->values.size(), 3u);
  EXPECT_DOUBLE_EQ(range->values[1], 4000.0);

  const auto list = parse_axis("salvo=1,2,4,8");
  ASSERT_TRUE(list.has_value());
  ASSERT_EQ(list->values.size(), 4u);
  EXPECT_DOUBLE_EQ(list->values[3], 8.0);

  EXPECT_FALSE(parse_axis("no-equals").has_value());
  EXPECT_FALSE(parse_axis("x=").has_value());
  EXPECT_FALSE(parse_axis("x=5:1:1").has_value());     // end < start.
  EXPECT_FALSE(parse_axis("x=1,banana").has_value());  // Numeric garbage.
}

TEST(ExperimentSmokeTest, UnknownAxisIsRejected) {
  CellParams cell;
  EXPECT_TRUE(apply_axis_value(cell, "salvo", 8.0));
  EXPECT_EQ(cell.salvo, 8);
  EXPECT_FALSE(apply_axis_value(cell, "warp_factor", 9.0));
}

TEST(ExperimentSmokeTest, OneCellRunsAndRowsCarryTheirSeeds) {
  std::string error;
  const auto scenario = sim::load_scenario_text(
      "weather_seed = 7\nduration_s = 40\n"
      "target_x = 0\ntarget_y = 5000\ntarget_z = 300\n"
      "target_heading_deg = 180\ntarget_speed = 240\n"
      "radar_scan_period_s = 0.5\n",
      &error);
  ASSERT_TRUE(scenario.has_value()) << error;

  CellParams cell;
  cell.scenario = *scenario;
  cell.salvo = 2;
  cell.settle_s = 4.0;

  for (const bool track_mode : {false, true}) {
    const auto rows = run_engagement(cell, track_mode, /*rep=*/1, /*base_seed=*/500);
    ASSERT_FALSE(rows.empty());
    for (const RunRow& row : rows) {
      EXPECT_EQ(row.sim_seed, 501u);
      EXPECT_EQ(row.gust_seed, 100501u);
      EXPECT_EQ(row.solver_track, track_mode);
    }
    ASSERT_TRUE(rows[0].fired) << "smoke geometry must produce a shot";
    EXPECT_EQ(rows.size(), 2u);  // One row per rocket.
    EXPECT_GT(rows[0].launch_tick, 0u);
    EXPECT_GT(rows[0].confirm_tick, 0u);
    EXPECT_GE(rows[0].miss_m, 0.0);
    if (track_mode) {
      EXPECT_GT(rows[0].track_error_at_launch_m, 0.0);
    }
  }

  // Reproducibility: the same (cell, rep) yields bit-identical rows.
  const auto again = run_engagement(cell, true, 1, 500);
  const auto first = run_engagement(cell, true, 1, 500);
  ASSERT_EQ(again.size(), first.size());
  for (std::size_t i = 0; i < again.size(); ++i) {
    EXPECT_EQ(again[i].miss_m, first[i].miss_m);
    EXPECT_EQ(again[i].launch_tick, first[i].launch_tick);
  }
}

}  // namespace
}  // namespace seashield::experiment
