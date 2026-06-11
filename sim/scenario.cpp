#include "sim/scenario.h"

#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>

namespace seashield::sim {

namespace {

std::string trim(const std::string& s) {
  const auto begin = s.find_first_not_of(" \t\r");
  if (begin == std::string::npos) {
    return "";
  }
  const auto end = s.find_last_not_of(" \t\r");
  return s.substr(begin, end - begin + 1);
}

class KeyValues {
 public:
  bool insert(const std::string& key, const std::string& value) {
    return values_.emplace(key, value).second;
  }

  bool has(const std::string& key) const { return values_.count(key) != 0; }

  double get_double(const std::string& key, double fallback) {
    auto it = values_.find(key);
    if (it == values_.end()) {
      return fallback;
    }
    consumed_.insert({it->first, true});
    char* end = nullptr;
    const double value = std::strtod(it->second.c_str(), &end);
    check_fully_parsed(key, it->second, end);
    return value;
  }

  std::uint64_t get_u64(const std::string& key, std::uint64_t fallback) {
    auto it = values_.find(key);
    if (it == values_.end()) {
      return fallback;
    }
    consumed_.insert({it->first, true});
    char* end = nullptr;
    const std::uint64_t value = std::strtoull(it->second.c_str(), &end, 10);
    check_fully_parsed(key, it->second, end);
    return value;
  }

  // Strict integer accessor (M-of-N counts and the like): the full-parse
  // check rejects "3.5" or trailing garbage the double path would mask.
  int get_int(const std::string& key, int fallback) {
    auto it = values_.find(key);
    if (it == values_.end()) {
      return fallback;
    }
    consumed_.insert({it->first, true});
    char* end = nullptr;
    const long value = std::strtol(it->second.c_str(), &end, 10);
    check_fully_parsed(key, it->second, end);
    return static_cast<int>(value);
  }

  // First key whose value contained numeric garbage ("target_speed = abc").
  const std::optional<std::string>& invalid_value_key() const { return invalid_value_key_; }

  // First key that was provided but never consumed (typo detection).
  std::optional<std::string> unconsumed_key() const {
    for (const auto& [key, value] : values_) {
      if (consumed_.count(key) == 0) {
        return key;
      }
    }
    return std::nullopt;
  }

 private:
  void check_fully_parsed(const std::string& key, const std::string& raw, const char* end) {
    if (!invalid_value_key_.has_value() && (end == raw.c_str() || *end != '\0')) {
      invalid_value_key_ = key;
    }
  }

  std::map<std::string, std::string> values_;
  std::map<std::string, bool> consumed_;
  std::optional<std::string> invalid_value_key_;
};

}  // namespace

std::optional<Scenario> load_scenario_text(const std::string& text, std::string* error) {
  const auto fail = [&](const std::string& message) -> std::optional<Scenario> {
    if (error != nullptr) {
      *error = message;
    }
    return std::nullopt;
  };

  KeyValues kv;
  std::istringstream stream(text);
  std::string line;
  int line_number = 0;
  while (std::getline(stream, line)) {
    ++line_number;
    const auto comment = line.find('#');
    if (comment != std::string::npos) {
      line = line.substr(0, comment);
    }
    line = trim(line);
    if (line.empty()) {
      continue;
    }
    const auto eq = line.find('=');
    if (eq == std::string::npos) {
      return fail("line " + std::to_string(line_number) + ": expected key = value");
    }
    const std::string key = trim(line.substr(0, eq));
    const std::string value = trim(line.substr(eq + 1));
    if (key.empty() || value.empty()) {
      return fail("line " + std::to_string(line_number) + ": empty key or value");
    }
    if (!kv.insert(key, value)) {
      return fail("line " + std::to_string(line_number) + ": duplicate key '" + key + "'");
    }
  }

  Scenario scenario;
  scenario.weather_seed = kv.get_u64("weather_seed", 1);
  scenario.duration_s = kv.get_double("duration_s", 60.0);

  WorldConfig& cfg = scenario.config;
  cfg.sim_seed = kv.get_u64("sim_seed", 1);
  cfg.gust_seed = kv.get_u64("gust_seed", 1);

  cfg.weather = WeatherGenerator::generate(scenario.weather_seed);

  // Explicit overrides (tests/experiments) on top of the generated weather.
  cfg.weather.gravity_mps2 = kv.get_double("gravity", cfg.weather.gravity_mps2);
  cfg.weather.sea_level_temperature_c =
      kv.get_double("temperature_c", cfg.weather.sea_level_temperature_c);
  cfg.weather.humidity = kv.get_double("humidity", cfg.weather.humidity);
  cfg.weather.rain_intensity = kv.get_double("rain", cfg.weather.rain_intensity);
  cfg.weather.turbulence_intensity =
      kv.get_double("turbulence", cfg.weather.turbulence_intensity);
  if (kv.has("surface_wind_speed") || kv.has("surface_wind_from_deg")) {
    const double speed = kv.get_double("surface_wind_speed", 0.0);
    const double from_rad = math::deg_to_rad(kv.get_double("surface_wind_from_deg", 0.0));
    // Meteorological FROM convention -> blowing-toward vector; uniform column.
    const math::Vec3 wind = math::Vec3{-math::sin(from_rad), -math::cos(from_rad), 0.0} * speed;
    cfg.weather.wind_layers = {{0.0, wind}, {6000.0, wind}};
  }

  cfg.target.initial_position.x = kv.get_double("target_x", 6000.0);
  cfg.target.initial_position.y = kv.get_double("target_y", 8000.0);
  cfg.target.initial_position.z = kv.get_double("target_z", 800.0);
  cfg.target.heading_rad = math::deg_to_rad(kv.get_double("target_heading_deg", 225.0));
  cfg.target.speed_mps = kv.get_double("target_speed", 250.0);
  cfg.target.turn_rate_rad_s = math::deg_to_rad(kv.get_double("target_turn_rate_deg", 0.0));

  // ASM maneuver profile (charter §5.3): range triggers default to 0 = off,
  // so legacy scenarios keep their exact behaviour.
  cfg.target.popup_range_m = kv.get_double("asm_popup_range_m", 0.0);
  cfg.target.popup_altitude_m = kv.get_double("asm_popup_altitude_m", cfg.target.popup_altitude_m);
  cfg.target.popup_climb_angle_rad = math::deg_to_rad(kv.get_double("asm_popup_climb_deg", 25.0));
  cfg.target.weave_range_m = kv.get_double("asm_weave_range_m", 0.0);
  cfg.target.weave_turn_rate_rad_s =
      math::deg_to_rad(kv.get_double("asm_weave_turn_deg_s", 15.0));
  cfg.target.weave_period_s = kv.get_double("asm_weave_period_s", cfg.target.weave_period_s);

  cfg.rocket.mass_kg = kv.get_double("rocket_mass", cfg.rocket.mass_kg);
  cfg.rocket.cda_m2 = kv.get_double("rocket_cda", cfg.rocket.cda_m2);
  cfg.rocket.thrust_n = kv.get_double("rocket_thrust", cfg.rocket.thrust_n);
  cfg.rocket.burn_time_s = kv.get_double("rocket_burn_time", cfg.rocket.burn_time_s);
  cfg.rocket.max_lifetime_s = kv.get_double("rocket_lifetime", cfg.rocket.max_lifetime_s);
  cfg.rocket.proximity_fuze_radius_m =
      kv.get_double("rocket_fuze_radius", cfg.rocket.proximity_fuze_radius_m);

  // Sensor chain (P4): radar and tracker tuning, all defaulted so legacy
  // scenarios run unchanged (the defaults live in Radar/TrackerParams).
  cfg.radar.scan_period_s = kv.get_double("radar_scan_period_s", cfg.radar.scan_period_s);
  cfg.radar.antenna_height_m = kv.get_double("radar_height_m", cfg.radar.antenna_height_m);
  cfg.radar.reference_range_m = kv.get_double("radar_ref_range_m", cfg.radar.reference_range_m);
  cfg.radar.pd_steepness_db = kv.get_double("radar_pd_steepness_db", cfg.radar.pd_steepness_db);
  cfg.radar.sigma_range_m = kv.get_double("radar_sigma_range_m", cfg.radar.sigma_range_m);
  if (kv.has("radar_sigma_az_mrad")) {
    cfg.radar.sigma_az_rad = math::mrad_to_rad(kv.get_double("radar_sigma_az_mrad", 0.0));
  }
  if (kv.has("radar_sigma_el_mrad")) {
    cfg.radar.sigma_el_rad = math::mrad_to_rad(kv.get_double("radar_sigma_el_mrad", 0.0));
  }
  cfg.radar.rain_atten_db_per_km =
      kv.get_double("radar_rain_atten_db_per_km", cfg.radar.rain_atten_db_per_km);

  cfg.tracker.accel_noise_mps2 = kv.get_double("track_accel_noise", cfg.tracker.accel_noise_mps2);
  cfg.tracker.gate_gamma = kv.get_double("track_gate_gamma", cfg.tracker.gate_gamma);
  cfg.tracker.confirm_m = kv.get_int("track_confirm_m", cfg.tracker.confirm_m);
  cfg.tracker.confirm_n = kv.get_int("track_confirm_n", cfg.tracker.confirm_n);
  cfg.tracker.drop_after_misses = kv.get_int("track_drop_misses", cfg.tracker.drop_after_misses);
  cfg.tracker.init_velocity_sigma_mps =
      kv.get_double("track_init_vel_sigma", cfg.tracker.init_velocity_sigma_mps);
  if (cfg.tracker.confirm_m < 1 || cfg.tracker.confirm_n < cfg.tracker.confirm_m ||
      cfg.tracker.confirm_n > 32 || cfg.tracker.drop_after_misses < 1) {
    return fail("invalid track confirmation parameters (need 1 <= M <= N <= 32, misses >= 1)");
  }

  if (const auto& invalid = kv.invalid_value_key()) {
    return fail("invalid numeric value for key '" + *invalid + "'");
  }
  if (const auto unknown = kv.unconsumed_key()) {
    return fail("unknown key '" + *unknown + "'");
  }
  return scenario;
}

std::optional<Scenario> load_scenario_file(const std::string& path, std::string* error) {
  std::ifstream in(path);
  if (!in.is_open()) {
    if (error != nullptr) {
      *error = "cannot open " + path;
    }
    return std::nullopt;
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return load_scenario_text(buffer.str(), error);
}

}  // namespace seashield::sim
