#include "protocol/reliable.h"

#include <algorithm>

namespace seashield::protocol {

// --- ReplayWindow -------------------------------------------------------------

ReplayWindow::Verdict ReplayWindow::check_and_set(std::uint16_t seq) {
  if (!initialized_) {
    initialized_ = true;
    latest_ = seq;
    bits_.fill(0);
    set_bit(seq);
    return Verdict::kFresh;
  }
  const std::int16_t diff = seq_diff(seq, latest_);
  if (diff > 0) {
    // Window advances: every sequence between the old latest and the new one
    // is now "not yet received" and its recycled bit slot must be cleared.
    if (diff >= static_cast<std::int16_t>(kBits)) {
      bits_.fill(0);
    } else {
      for (std::int16_t i = 1; i <= diff; ++i) {
        clear_bit(static_cast<std::uint16_t>(latest_ + i));
      }
    }
    latest_ = seq;
    set_bit(seq);
    return Verdict::kFresh;
  }
  if (diff <= -static_cast<std::int16_t>(kBits)) {
    return Verdict::kStale;
  }
  if (bit(seq)) {
    return Verdict::kDuplicate;
  }
  set_bit(seq);
  return Verdict::kFresh;
}

bool ReplayWindow::was_received(std::uint16_t seq) const {
  if (!initialized_) {
    return false;
  }
  const std::int16_t diff = seq_diff(latest_, seq);  // How far behind latest.
  if (diff < 0 || diff >= static_cast<std::int16_t>(kBits)) {
    return false;
  }
  return bit(seq);
}

// --- ReliableEndpoint ----------------------------------------------------------

ReliableEndpoint::ReliableEndpoint(EndpointConfig config) : config_(config) {}

double ReliableEndpoint::rto_s() const {
  if (srtt_s_ <= 0.0) {
    return config_.rto_initial_s;
  }
  return std::clamp(srtt_s_ + 4.0 * rttvar_s_, config_.rto_min_s, config_.rto_max_s);
}

void ReliableEndpoint::add_rtt_sample(double sample_s) {
  if (sample_s < 0.0) {
    return;
  }
  if (srtt_s_ <= 0.0) {
    srtt_s_ = sample_s;
    rttvar_s_ = sample_s / 2.0;
    return;
  }
  const double err = srtt_s_ - sample_s;
  rttvar_s_ = 0.75 * rttvar_s_ + 0.25 * (err < 0.0 ? -err : err);
  srtt_s_ = 0.875 * srtt_s_ + 0.125 * sample_s;
}

void ReliableEndpoint::mark_acked(std::uint16_t seq, double now_s) {
  SentPacketSlot& slot = sent_[seq % sent_.size()];
  if (!slot.used || slot.seq != seq || slot.acked) {
    return;  // Never sent, recycled slot, or already processed.
  }
  slot.acked = true;
  add_rtt_sample(now_s - slot.sent_time_s);
  for (const std::uint16_t id : slot.msg_ids) {
    in_flight_.erase(id);
  }
  slot.msg_ids.clear();
}

void ReliableEndpoint::write_header(Writer& w, Channel channel, std::uint16_t seq) const {
  w.u16(kPacketMagic);
  w.u8(kProtocolVersion);
  // Until something has been received there IS no ack state; advertising
  // ack=0 then would fake-acknowledge the peer's genuine packet 0.
  const std::uint8_t ack_valid = packet_window_.initialized() ? kAckValidFlag : 0;
  w.u8(static_cast<std::uint8_t>(channel) | ack_valid);
  w.u16(seq);
  w.u16(packet_window_.latest());
  std::uint32_t ack_bits = 0;
  if (packet_window_.initialized()) {
    for (std::uint32_t i = 0; i < 32; ++i) {
      const auto seq_i = static_cast<std::uint16_t>(packet_window_.latest() - 1 - i);
      if (packet_window_.was_received(seq_i)) {
        ack_bits |= (1u << i);
      }
    }
  }
  w.u32(ack_bits);
}

std::uint16_t ReliableEndpoint::record_sent_packet(double now_s,
                                                   std::vector<std::uint16_t> msg_ids) {
  const std::uint16_t seq = next_seq_++;
  SentPacketSlot& slot = sent_[seq % sent_.size()];
  slot.seq = seq;
  slot.used = true;
  slot.acked = false;
  slot.sent_time_s = now_s;
  slot.msg_ids = std::move(msg_ids);
  return seq;
}

bool ReliableEndpoint::on_datagram(double now_s, std::span<const std::uint8_t> datagram,
                                   const MessageHandler& handler) {
  Reader r(datagram);
  const std::uint16_t magic = r.u16();
  const std::uint8_t version = r.u8();
  const std::uint8_t channel_raw = r.u8();
  const std::uint16_t seq = r.u16();
  const std::uint16_t ack = r.u16();
  const std::uint32_t ack_bits = r.u32();
  const bool ack_valid = (channel_raw & kAckValidFlag) != 0;
  const std::uint8_t channel_bits = channel_raw & ~kAckValidFlag;
  if (!r.ok() || magic != kPacketMagic || version != kProtocolVersion ||
      channel_bits > static_cast<std::uint8_t>(Channel::kReliable)) {
    ++stats_.packets_malformed;
    return false;
  }

  // Packet-level dedup: an exact duplicate datagram (network dup or attacker
  // replay) is dropped whole — its acks were applied the first time around.
  switch (packet_window_.check_and_set(seq)) {
    case ReplayWindow::Verdict::kDuplicate:
      ++stats_.packets_duplicate;
      return false;
    case ReplayWindow::Verdict::kStale:
      ++stats_.packets_stale;
      return false;
    case ReplayWindow::Verdict::kFresh:
      break;
  }
  ++stats_.packets_received;

  // Piggybacked acks: ack itself plus one bit per predecessor (charter §4.4,
  // "최근 33개"). Unknown sequences are ignored by mark_acked.
  if (ack_valid) {
    mark_acked(ack, now_s);
    for (std::uint32_t i = 0; i < 32; ++i) {
      if (ack_bits & (1u << i)) {
        mark_acked(static_cast<std::uint16_t>(ack - 1 - i), now_s);
      }
    }
  }

  const auto channel = static_cast<Channel>(channel_bits);
  if (channel == Channel::kReliable) {
    // The sender is waiting on an ack to stop retransmitting; promise one
    // within ack_delay_s even if we have no traffic of our own. Unreliable
    // packets never set this — otherwise two idle endpoints would ack each
    // other's acks forever.
    if (!ack_needed_) {
      ack_needed_ = true;
      ack_deadline_s_ = now_s + config_.ack_delay_s;
    }
  }

  while (r.ok() && r.remaining() > 0) {
    const auto type = static_cast<MsgType>(r.u8());
    const std::uint16_t len = r.u16();
    const auto body = r.bytes(len);
    if (!r.ok()) {
      ++stats_.packets_malformed;
      return false;
    }
    if (channel == Channel::kReliable) {
      Reader body_reader(body);
      const std::uint16_t msg_id = body_reader.u16();
      if (!body_reader.ok()) {
        ++stats_.packets_malformed;
        return false;
      }
      if (reliable_window_.check_and_set(msg_id) != ReplayWindow::Verdict::kFresh) {
        ++stats_.messages_deduplicated;  // Retransmission overlap — expected.
        continue;
      }
      handler(type, body.subspan(2));
    } else {
      handler(type, body);
    }
  }
  return true;
}

void ReliableEndpoint::send_unreliable(MsgType type, std::span<const std::uint8_t> payload) {
  if (3 + payload.size() > payload_budget()) {
    ++stats_.unreliable_dropped_oversize;
    return;
  }
  Writer w;
  w.u8(static_cast<std::uint8_t>(type));
  w.u16(static_cast<std::uint16_t>(payload.size()));
  w.bytes(payload);
  pending_unreliable_.push_back(w.take());
}

bool ReliableEndpoint::send_reliable(MsgType type, std::span<const std::uint8_t> payload) {
  if (3 + 2 + payload.size() > payload_budget() ||
      in_flight_.size() >= config_.max_in_flight_messages) {
    return false;
  }
  const std::uint16_t msg_id = next_msg_id_++;
  Writer w;
  w.u8(static_cast<std::uint8_t>(type));
  w.u16(static_cast<std::uint16_t>(2 + payload.size()));
  w.u16(msg_id);
  w.bytes(payload);
  in_flight_.emplace(msg_id, InFlightMessage{w.take(), -1.0, -1.0});
  ++stats_.reliable_sent;
  return true;
}

void ReliableEndpoint::flush(double now_s, const DatagramSink& sink) {
  bool sent_any = false;

  // Reliable channel: bundle everything never sent or past its RTO. A
  // retransmission rides in a brand-new packet (new seq); the old packet
  // record simply never gets acked.
  const double rto = rto_s();
  std::vector<std::uint16_t> due;
  for (const auto& [id, msg] : in_flight_) {
    if (msg.last_sent_s < 0.0 || now_s - msg.last_sent_s >= rto) {
      due.push_back(id);
    }
  }
  std::size_t i = 0;
  while (i < due.size()) {
    Writer w;
    write_header(w, Channel::kReliable, next_seq_);  // Seq consumed below.
    std::vector<std::uint16_t> packed;
    while (i < due.size()) {
      InFlightMessage& msg = in_flight_.at(due[i]);
      if (w.size() + msg.envelope.size() > config_.max_datagram_bytes) {
        break;
      }
      w.bytes(msg.envelope);
      if (msg.last_sent_s >= 0.0) {
        ++stats_.retransmissions;
      } else {
        msg.first_sent_s = now_s;
      }
      msg.last_sent_s = now_s;
      packed.push_back(due[i]);
      ++i;
    }
    record_sent_packet(now_s, std::move(packed));
    ++stats_.packets_sent;
    sent_any = true;
    sink(w.data());
  }

  // Unreliable channel: greedy FIFO bundling.
  std::size_t u = 0;
  while (u < pending_unreliable_.size()) {
    Writer w;
    write_header(w, Channel::kUnreliable, next_seq_);
    while (u < pending_unreliable_.size() &&
           w.size() + pending_unreliable_[u].size() <= config_.max_datagram_bytes) {
      w.bytes(pending_unreliable_[u]);
      ++u;
    }
    record_sent_packet(now_s, {});
    ++stats_.packets_sent;
    sent_any = true;
    sink(w.data());
  }
  pending_unreliable_.clear();

  if (sent_any) {
    ack_needed_ = false;
  } else if (ack_needed_ && now_s >= ack_deadline_s_) {
    // Ack-only packet: empty unreliable packet whose header does the work.
    Writer w;
    write_header(w, Channel::kUnreliable, next_seq_);
    record_sent_packet(now_s, {});
    ++stats_.packets_sent;
    ack_needed_ = false;
    sink(w.data());
  }
}

bool ReliableEndpoint::peer_timed_out(double now_s) const {
  for (const auto& [id, msg] : in_flight_) {
    if (msg.first_sent_s >= 0.0 && now_s - msg.first_sent_s > config_.peer_timeout_s) {
      return true;
    }
  }
  return false;
}

}  // namespace seashield::protocol
