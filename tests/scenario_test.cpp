#include "sim/scenario.h"

#include <gtest/gtest.h>

namespace seashield::sim {
namespace {

TEST(ScenarioTest, ParsesFullScenario) {
  const std::string text = R"(
# full scenario
weather_seed = 42
sim_seed = 7
gust_seed = 9
duration_s = 90

target_x = 5000
target_y = 1000   # inline comment
target_z = 400
target_heading_deg = 90
target_speed = 220
target_turn_rate_deg = 3

rocket_mass = 55
rocket_fuze_radius = 15

temperature_c = 30
surface_wind_speed = 8
surface_wind_from_deg = 90
turbulence = 0.2
)";
  std::string error;
  const auto scenario = load_scenario_text(text, &error);
  ASSERT_TRUE(scenario.has_value()) << error;
  EXPECT_EQ(scenario->weather_seed, 42u);
  EXPECT_DOUBLE_EQ(scenario->duration_s, 90.0);
  const WorldConfig& cfg = scenario->config;
  EXPECT_EQ(cfg.sim_seed, 7u);
  EXPECT_EQ(cfg.gust_seed, 9u);
  EXPECT_DOUBLE_EQ(cfg.target.initial_position.x, 5000.0);
  EXPECT_DOUBLE_EQ(cfg.target.speed_mps, 220.0);
  EXPECT_NEAR(cfg.target.turn_rate_rad_s, math::deg_to_rad(3.0), 1e-12);
  EXPECT_DOUBLE_EQ(cfg.rocket.mass_kg, 55.0);
  EXPECT_DOUBLE_EQ(cfg.rocket.proximity_fuze_radius_m, 15.0);
  // Overrides on top of the generated weather.
  EXPECT_DOUBLE_EQ(cfg.weather.sea_level_temperature_c, 30.0);
  EXPECT_DOUBLE_EQ(cfg.weather.turbulence_intensity, 0.2);
  // Wind FROM East (90°) blows toward -x (West); uniform column.
  ASSERT_EQ(cfg.weather.wind_layers.size(), 2u);
  EXPECT_NEAR(cfg.weather.wind_layers[0].velocity.x, -8.0, 1e-9);
  EXPECT_NEAR(cfg.weather.wind_layers[0].velocity.y, 0.0, 1e-9);
}

TEST(ScenarioTest, DefaultsApplyWhenOmitted) {
  const auto scenario = load_scenario_text("weather_seed = 5\n", nullptr);
  ASSERT_TRUE(scenario.has_value());
  EXPECT_DOUBLE_EQ(scenario->config.target.initial_position.x, 6000.0);
  EXPECT_DOUBLE_EQ(scenario->config.target.speed_mps, 250.0);
  EXPECT_DOUBLE_EQ(scenario->duration_s, 60.0);
  // Without overrides the generated weather is kept untouched.
  const Weather generated = WeatherGenerator::generate(5);
  EXPECT_DOUBLE_EQ(scenario->config.weather.sea_level_temperature_c,
                   generated.sea_level_temperature_c);
}

TEST(ScenarioTest, UnknownKeyIsRejected) {
  std::string error;
  EXPECT_FALSE(load_scenario_text("target_xx = 1\n", &error).has_value());
  EXPECT_NE(error.find("target_xx"), std::string::npos);
}

TEST(ScenarioTest, DuplicateKeyIsRejected) {
  std::string error;
  EXPECT_FALSE(load_scenario_text("sim_seed = 1\nsim_seed = 2\n", &error).has_value());
  EXPECT_NE(error.find("duplicate"), std::string::npos);
}

TEST(ScenarioTest, MalformedLineIsRejected) {
  std::string error;
  EXPECT_FALSE(load_scenario_text("just words\n", &error).has_value());
  EXPECT_NE(error.find("line 1"), std::string::npos);
}

}  // namespace
}  // namespace seashield::sim
