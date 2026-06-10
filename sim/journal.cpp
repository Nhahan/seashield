#include "sim/journal.h"

#include <cinttypes>
#include <cstdio>
#include <sstream>

namespace seashield::sim {

std::string Journal::serialize() const {
  std::string out;
  char buf[256];
  for (const JournalEntry& e : entries_) {
    std::snprintf(buf, sizeof(buf),
                  "fire tick=%" PRIu64 " az=%.17g el=%.17g salvo=%d dispersion=%.17g "
                  "interval=%.17g\n",
                  e.tick, e.command.azimuth_rad, e.command.elevation_rad,
                  e.command.salvo_count, e.command.dispersion_mrad,
                  e.command.launch_interval_s);
    out += buf;
  }
  return out;
}

std::optional<Journal> Journal::parse(const std::string& text) {
  Journal journal;
  std::istringstream stream(text);
  std::string line;
  while (std::getline(stream, line)) {
    if (line.empty()) {
      continue;
    }
    JournalEntry e;
    const int matched = std::sscanf(
        line.c_str(),
        "fire tick=%" SCNu64 " az=%lg el=%lg salvo=%d dispersion=%lg interval=%lg", &e.tick,
        &e.command.azimuth_rad, &e.command.elevation_rad, &e.command.salvo_count,
        &e.command.dispersion_mrad, &e.command.launch_interval_s);
    if (matched != 6) {
      return std::nullopt;
    }
    journal.entries_.push_back(e);
  }
  return journal;
}

}  // namespace seashield::sim
