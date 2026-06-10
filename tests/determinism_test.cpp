#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "sim/journal.h"
#include "sim/world.h"

namespace seashield::sim {
namespace {

WorldConfig canonical_config() {
  WorldConfig cfg;
  cfg.weather = WeatherGenerator::generate(42);
  cfg.target.initial_position = {6000, 8000, 800};
  cfg.target.heading_rad = math::deg_to_rad(225.0);
  cfg.target.speed_mps = 250.0;
  cfg.sim_seed = 7;
  cfg.gust_seed = 9;
  return cfg;
}

FireCommand canonical_fire() {
  FireCommand cmd;
  cmd.azimuth_rad = math::deg_to_rad(36.0);
  cmd.elevation_rad = math::deg_to_rad(30.0);
  cmd.salvo_count = 4;
  cmd.dispersion_mrad = 4.0;
  return cmd;
}

// Runs the canonical scenario for 600 ticks, sampling the hash every 60.
std::vector<std::uint64_t> run_canonical(const WorldConfig& cfg) {
  World world(cfg);
  std::vector<std::uint64_t> hashes;
  for (std::uint64_t t = 0; t < 600; ++t) {
    if (t == 30 || t == 240) {
      world.queue_fire(canonical_fire());
    }
    world.step();
    if (world.tick() % 60 == 0) {
      hashes.push_back(world.state_hash());
    }
  }
  return hashes;
}

TEST(DeterminismTest, IdenticalRunsProduceIdenticalHashes) {
  const auto a = run_canonical(canonical_config());
  const auto b = run_canonical(canonical_config());
  ASSERT_EQ(a.size(), b.size());
  for (std::size_t i = 0; i < a.size(); ++i) {
    EXPECT_EQ(a[i], b[i]) << "hash diverged at sample " << i;
  }
}

TEST(DeterminismTest, DifferentGustSeedDiverges) {
  WorldConfig other = canonical_config();
  other.gust_seed = 10;
  EXPECT_NE(run_canonical(canonical_config()).back(), run_canonical(other).back());
}

TEST(DeterminismTest, JournalSerializationRoundTripsBitExactly) {
  Journal journal;
  FireCommand cmd;
  cmd.azimuth_rad = 0.6154797086703873;  // Deliberately non-round doubles.
  cmd.elevation_rad = 0.5235987755982988;
  cmd.salvo_count = 4;
  cmd.dispersion_mrad = 4.123456789012345;
  cmd.launch_interval_s = 1.0 / 30.0;
  journal.record(30, cmd);
  cmd.salvo_count = 2;
  journal.record(240, cmd);

  const auto parsed = Journal::parse(journal.serialize());
  ASSERT_TRUE(parsed.has_value());
  ASSERT_EQ(parsed->entries().size(), 2u);
  for (std::size_t i = 0; i < 2; ++i) {
    const JournalEntry& a = journal.entries()[i];
    const JournalEntry& b = parsed->entries()[i];
    EXPECT_EQ(a.tick, b.tick);
    EXPECT_EQ(a.command.salvo_count, b.command.salvo_count);
    // %.17g round-trips IEEE doubles exactly.
    EXPECT_DOUBLE_EQ(a.command.azimuth_rad, b.command.azimuth_rad);
    EXPECT_DOUBLE_EQ(a.command.elevation_rad, b.command.elevation_rad);
    EXPECT_DOUBLE_EQ(a.command.dispersion_mrad, b.command.dispersion_mrad);
    EXPECT_DOUBLE_EQ(a.command.launch_interval_s, b.command.launch_interval_s);
  }
}

TEST(DeterminismTest, ReplayFromJournalMatchesOriginalRun) {
  // Original run records its inputs.
  Journal journal;
  World original(canonical_config());
  for (std::uint64_t t = 0; t < 600; ++t) {
    if (t == 30 || t == 240) {
      journal.record(t, canonical_fire());
      original.queue_fire(canonical_fire());
    }
    original.step();
  }

  // Replay: same config, inputs applied from the parsed journal text.
  const auto parsed = Journal::parse(journal.serialize());
  ASSERT_TRUE(parsed.has_value());
  World replay(canonical_config());
  std::size_t next_entry = 0;
  for (std::uint64_t t = 0; t < 600; ++t) {
    while (next_entry < parsed->entries().size() && parsed->entries()[next_entry].tick == t) {
      replay.queue_fire(parsed->entries()[next_entry].command);
      ++next_entry;
    }
    replay.step();
  }
  EXPECT_EQ(original.state_hash(), replay.state_hash());
}

std::string golden_path() {
#if defined(__APPLE__)
  const char* os = "darwin";
#elif defined(__linux__)
  const char* os = "linux";
#else
  const char* os = "unknown";
#endif
#if defined(__aarch64__) || defined(__arm64__)
  const char* arch = "arm64";
#elif defined(__x86_64__)
  const char* arch = "x86_64";
#else
  const char* arch = "unknown";
#endif
  return std::string(SEASHIELD_SOURCE_DIR) + "/tests/golden/determinism-" + os + "-" + arch +
         ".txt";
}

// Bit-level regression across builds of the same platform/toolchain family.
// Cross-platform identity is a documented non-goal (charter §5.1), so golden
// files are per-(os, arch). Regenerate with SEASHIELD_UPDATE_GOLDEN=1.
TEST(DeterminismTest, GoldenHashRegression) {
  const auto hashes = run_canonical(canonical_config());
  const std::string path = golden_path();

  if (std::getenv("SEASHIELD_UPDATE_GOLDEN") != nullptr) {
    std::ofstream out(path);
    ASSERT_TRUE(out.is_open());
    for (const std::uint64_t h : hashes) {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%016llx\n", static_cast<unsigned long long>(h));
      out << buf;
    }
    GTEST_SKIP() << "golden updated: " << path;
  }

  std::ifstream in(path);
  if (!in.is_open()) {
    GTEST_SKIP() << "no golden for this platform: " << path;
  }
  std::vector<std::string> expected;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty()) {
      expected.push_back(line);
    }
  }
  ASSERT_EQ(expected.size(), hashes.size());
  for (std::size_t i = 0; i < hashes.size(); ++i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(hashes[i]));
    EXPECT_EQ(expected[i], buf) << "hash regression at sample " << i;
  }
}

}  // namespace
}  // namespace seashield::sim
