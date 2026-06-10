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
    return std::strtod(it->second.c_str(), nullptr);
  }

  std::uint64_t get_u64(const std::string& key, std::uint64_t fallback) {
    auto it = values_.find(key);
    if (it == values_.end()) {
      return fallback;
    }
    consumed_.insert({it->first, true});
    return std::strtoull(it->second.c_str(), nullptr, 10);
  }

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
  std::map<std::string, std::string> values_;
  std::map<std::string, bool> consumed_;
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

  cfg.rocket.mass_kg = kv.get_double("rocket_mass", cfg.rocket.mass_kg);
  cfg.rocket.cda_m2 = kv.get_double("rocket_cda", cfg.rocket.cda_m2);
  cfg.rocket.thrust_n = kv.get_double("rocket_thrust", cfg.rocket.thrust_n);
  cfg.rocket.burn_time_s = kv.get_double("rocket_burn_time", cfg.rocket.burn_time_s);
  cfg.rocket.max_lifetime_s = kv.get_double("rocket_lifetime", cfg.rocket.max_lifetime_s);
  cfg.rocket.proximity_fuze_radius_m =
      kv.get_double("rocket_fuze_radius", cfg.rocket.proximity_fuze_radius_m);

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
