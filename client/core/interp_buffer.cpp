#include "client/core/interp_buffer.h"

#include <algorithm>

namespace seashield::client {

namespace {

// Snapshot identity is (kind, id): rocket ids and track ids share a number
// space on the wire, so the kind disambiguates.
std::uint32_t entity_key(const protocol::EntityRecord& entity) {
  return (static_cast<std::uint32_t>(entity.kind) << 16) | entity.id;
}

}  // namespace

std::optional<CompletedSnapshot> SnapshotAssembler::complete(CompletedSnapshot done) {
  partials_.erase(partials_.begin(), partials_.upper_bound(done.tick));
  delta_partials_.erase(delta_partials_.begin(), delta_partials_.upper_bound(done.tick));
  newest_completed_ = done.tick;
  has_completed_ = true;
  baselines_[done.tick] = done;  // Copy: kept as a future delta baseline.
  while (baselines_.size() > kMaxBaselines) {
    baselines_.erase(baselines_.begin());
  }
  return done;
}

std::optional<CompletedSnapshot> SnapshotAssembler::push(const protocol::Snapshot& batch) {
  if (stale(batch.tick)) {
    partials_.erase(batch.tick);
    return std::nullopt;  // Stale: a finished frame must never rewind the view.
  }
  Partial& partial = partials_[batch.tick];
  if (partial.filled.empty() && partial.received == 0) {
    partial.total = batch.total_entities;
    partial.phase = batch.phase;
    partial.entities.resize(batch.total_entities);
    partial.filled.assign(batch.total_entities, false);
  } else if (batch.total_entities != partial.total) {
    partials_.erase(batch.tick);  // Inconsistent stream — drop the whole frame.
    return std::nullopt;
  }
  for (std::size_t i = 0; i < batch.entities.size(); ++i) {
    const std::size_t index = static_cast<std::size_t>(batch.first_index) + i;
    if (index >= partial.total) {
      break;  // Malformed index range; keep what fits.
    }
    if (!partial.filled[index]) {
      partial.filled[index] = true;
      partial.entities[index] = batch.entities[i];
      ++partial.received;
    }
  }
  if (partial.received < partial.total) {
    while (partials_.size() > kMaxPendingTicks) {
      partials_.erase(partials_.begin());  // Loss bound: shed the oldest frame.
    }
    return std::nullopt;
  }

  CompletedSnapshot done;
  done.tick = batch.tick;
  done.phase = partial.phase;
  done.entities = std::move(partial.entities);
  return complete(std::move(done));
}

std::optional<CompletedSnapshot> SnapshotAssembler::push_delta(const protocol::SnapshotDelta& batch) {
  if (stale(batch.tick)) {
    delta_partials_.erase(batch.tick);
    return std::nullopt;
  }
  DeltaPartial& partial = delta_partials_[batch.tick];
  if (partial.filled.empty() && partial.received == 0) {
    partial.total = batch.total_entities;
    partial.base_tick = batch.base_tick;
    partial.phase = batch.phase;
    partial.entities.resize(batch.total_entities);
    partial.filled.assign(batch.total_entities, false);
  } else if (batch.total_entities != partial.total || batch.base_tick != partial.base_tick) {
    delta_partials_.erase(batch.tick);  // Inconsistent stream — drop the frame.
    return std::nullopt;
  }
  for (std::size_t i = 0; i < batch.entities.size(); ++i) {
    const std::size_t index = static_cast<std::size_t>(batch.first_index) + i;
    if (index >= partial.total) {
      break;
    }
    if (!partial.filled[index]) {
      partial.filled[index] = true;
      partial.entities[index] = batch.entities[i];
      ++partial.received;
    }
  }
  if (partial.received < partial.total) {
    while (delta_partials_.size() > kMaxPendingTicks) {
      delta_partials_.erase(delta_partials_.begin());
    }
    return std::nullopt;
  }

  CompletedSnapshot done;
  done.tick = batch.tick;
  done.phase = partial.phase;
  if (partial.total == 0) {
    delta_partials_.erase(batch.tick);
    return complete(std::move(done));
  }

  // Reconstruct against the acked baseline. A missing baseline (evicted, or
  // we never completed it) drops the frame — the server's full-snapshot
  // fallback heals the stream once our acks stop advancing.
  const auto base_it = baselines_.find(partial.base_tick);
  if (base_it == baselines_.end()) {
    delta_partials_.erase(batch.tick);
    return std::nullopt;
  }
  std::map<std::uint32_t, const protocol::EntityRecord*> base_index;
  for (const protocol::EntityRecord& entity : base_it->second.entities) {
    base_index[entity_key(entity)] = &entity;
  }
  const std::uint32_t dticks = batch.tick - partial.base_tick;
  done.entities.reserve(partial.total);
  for (const protocol::DeltaEntity& delta : partial.entities) {
    if ((delta.mask & protocol::DeltaEntity::kFullRecord) != 0) {
      done.entities.push_back(delta.full);
      continue;
    }
    const std::uint32_t key =
        (static_cast<std::uint32_t>(delta.kind()) << 16) | delta.id;
    const auto base_entity = base_index.find(key);
    if (base_entity == base_index.end()) {
      delta_partials_.erase(batch.tick);  // Residual without a base: corrupt frame.
      return std::nullopt;
    }
    done.entities.push_back(
        protocol::apply_delta_entity(*base_entity->second, delta, dticks, tick_rate_hz_));
  }
  delta_partials_.erase(batch.tick);
  return complete(std::move(done));
}

void InterpolationBuffer::push(CompletedSnapshot snapshot) {
  if (!history_.empty() && snapshot.tick <= history_.back().tick) {
    return;  // The assembler already filters stale frames; belt and braces.
  }
  history_.push_back(std::move(snapshot));
  while (history_.size() > kMaxHistory) {
    history_.pop_front();
  }
}

std::optional<double> InterpolationBuffer::render_tick(double delay_ticks) const {
  if (history_.empty()) {
    return std::nullopt;
  }
  return static_cast<double>(history_.back().tick) - delay_ticks;
}

std::vector<SampledEntity> InterpolationBuffer::sample(double tick_time) const {
  std::vector<SampledEntity> out;
  if (history_.empty()) {
    return out;
  }
  // Bracket the sample time: a = newest snapshot at or before it (clamped to
  // the oldest), b = the one after (nullptr when sampling past the newest).
  const CompletedSnapshot* a = &history_.front();
  const CompletedSnapshot* b = nullptr;
  for (const CompletedSnapshot& snap : history_) {
    if (static_cast<double>(snap.tick) <= tick_time) {
      a = &snap;
    } else {
      b = &snap;
      break;
    }
  }

  std::map<std::uint32_t, const protocol::EntityRecord*> next;
  if (b != nullptr) {
    for (const protocol::EntityRecord& entity : b->entities) {
      next[entity_key(entity)] = &entity;
    }
  }

  const double dt_ticks = tick_time - static_cast<double>(a->tick);
  out.reserve(a->entities.size());
  for (const protocol::EntityRecord& entity : a->entities) {
    SampledEntity sampled;
    sampled.id = entity.id;
    sampled.kind = entity.kind;
    sampled.state = entity.state;
    sampled.flags = entity.flags;
    sampled.pos_x = entity.pos_x;
    sampled.pos_y = entity.pos_y;
    sampled.pos_z = entity.pos_z;
    sampled.vel_x = entity.vel_x;
    sampled.vel_y = entity.vel_y;
    sampled.vel_z = entity.vel_z;

    const protocol::EntityRecord* upcoming = nullptr;
    if (b != nullptr) {
      const auto it = next.find(entity_key(entity));
      upcoming = it != next.end() ? it->second : nullptr;
    }

    if (entity.kind == protocol::EntityKind::kTrack || dt_ticks <= 0.0) {
      // Tracks snap to the newest estimate at or before the sample time.
    } else if (upcoming != nullptr) {
      const double span = static_cast<double>(b->tick - a->tick);
      const double alpha = std::clamp(dt_ticks / span, 0.0, 1.0);
      sampled.pos_x += alpha * (upcoming->pos_x - entity.pos_x);
      sampled.pos_y += alpha * (upcoming->pos_y - entity.pos_y);
      sampled.pos_z += alpha * (upcoming->pos_z - entity.pos_z);
      sampled.vel_x += alpha * (upcoming->vel_x - entity.vel_x);
      sampled.vel_y += alpha * (upcoming->vel_y - entity.vel_y);
      sampled.vel_z += alpha * (upcoming->vel_z - entity.vel_z);
    } else {
      // Gone from the next frame, or no next frame yet: dead-reckon along
      // the velocity, capped so a stalled feed freezes instead of flying off.
      const double dt_s =
          std::min(dt_ticks, max_extrapolation_ticks_) / tick_rate_hz_;
      sampled.pos_x += entity.vel_x * dt_s;
      sampled.pos_y += entity.vel_y * dt_s;
      sampled.pos_z += entity.vel_z * dt_s;
      sampled.extrapolated = true;
    }
    out.push_back(sampled);
  }
  return out;
}

}  // namespace seashield::client
