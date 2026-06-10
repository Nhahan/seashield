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
  std::uint64_t tick = 0;
  FireCommand command;
};

class Journal {
 public:
  void record(std::uint64_t tick, const FireCommand& command) {
    entries_.push_back({tick, command});
  }

  const std::vector<JournalEntry>& entries() const { return entries_; }

  // Text form; doubles use %.17g so parse() round-trips bit-exactly.
  std::string serialize() const;
  static std::optional<Journal> parse(const std::string& text);

 private:
  std::vector<JournalEntry> entries_;
};

}  // namespace seashield::sim
