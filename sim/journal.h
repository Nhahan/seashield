#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "sim/world.h"

// Input journal (charter §5.8): the complete record of external inputs with
// the tick they were applied at. Determinism makes (config seeds + journal)
// sufficient to reproduce an entire engagement — this is the replay format.
namespace seashield::sim {

struct JournalEntry {
  enum class Kind : std::uint8_t { kFire = 0, kSteer = 1 };
  std::uint64_t tick = 0;
  Kind kind = Kind::kFire;
  FireCommand command;   // Valid when kind == kFire.
  SteerCommand steer;    // Valid when kind == kSteer (own-ship maneuver).
};

class Journal {
 public:
  void record(std::uint64_t tick, const FireCommand& command) {
    entries_.push_back({tick, JournalEntry::Kind::kFire, command, {}});
  }
  void record_steer(std::uint64_t tick, const SteerCommand& steer) {
    entries_.push_back({tick, JournalEntry::Kind::kSteer, {}, steer});
  }

  const std::vector<JournalEntry>& entries() const { return entries_; }

  // Text form; doubles use %.17g so parse() round-trips bit-exactly.
  // The line format is version-locked to this binary: field order and count
  // are fixed, and a future format change must bump a version token rather
  // than extend lines in place.
  std::string serialize() const;
  static std::optional<Journal> parse(const std::string& text);

 private:
  std::vector<JournalEntry> entries_;
};

}  // namespace seashield::sim
