#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <vector>

#include "protocol/messages.h"
#include "protocol/wire.h"

// Selective-reliability UDP layer (charter §4.4): NOT a TCP reimplementation.
// Every datagram is one packet:
//
//   [magic u16][version u8][channel u8][seq u16][ack u16][ack_bits u32]  = 12B
//   then messages back-to-back:  [type u8][len u16][body len bytes]
//   (Reliable channel bodies are [msg_id u16][payload]; msg_id is what the
//    receiver dedups on, since a retransmission travels in a NEW packet.)
//
// The channel byte is `channel id | 0x80` once the sender has received at
// least one packet: bit 7 says the ack/ack_bits fields are meaningful. Without
// it, an endpoint that has heard nothing yet would emit ack=0 and silently
// fake-acknowledge the peer's real packet 0 — found by the chaos sweep test.
//
// One sequence space covers all packets of a connection regardless of
// channel; ack/ack_bits piggyback the receive state of the last 33 packets on
// every outgoing packet (charter §4.4). Reliable-Ordered is intentionally
// unsupported — data that needs total order goes over TCP.
//
// Retransmission: a reliable message stays "in flight" until a packet that
// carried it is acked; when its last carrier is older than the RTO (RFC
// 6298-style estimator over piggybacked acks), it is re-bundled into the next
// packet with a fresh sequence number. Re-sending under a new seq sidesteps
// retransmission ambiguity (Karn), so every ack is a clean RTT sample.
//
// Pure logic: no sockets, no wall clock — the caller injects `now_s` and a
// datagram sink, which is what makes the loss/reorder property tests
// deterministic (charter §10.3, P3a gate).
namespace seashield::protocol {

inline constexpr std::uint16_t kPacketMagic = 0x5EA5;
inline constexpr std::size_t kPacketHeaderBytes = 12;
// Bit 7 of the channel byte: the ack/ack_bits fields hold real receive state.
inline constexpr std::uint8_t kAckValidFlag = 0x80;

enum class Channel : std::uint8_t {
  kUnreliable = 0,
  kReliable = 1,
};

// Wrap-around-safe sequence comparison (charter §4.4): the int16_t difference
// puts b in a's ±32767 half-window. a is "newer" iff the signed gap is > 0.
constexpr std::int16_t seq_diff(std::uint16_t a, std::uint16_t b) {
  return static_cast<std::int16_t>(static_cast<std::uint16_t>(a - b));
}
constexpr bool seq_newer(std::uint16_t a, std::uint16_t b) { return seq_diff(a, b) > 0; }

// Sliding receive window over a 16-bit sequence space (packets AND reliable
// message ids use one each). Remembers the most recent kBits sequences;
// anything older is reported stale and dropped — with in-flight caps far
// below kBits, a live peer can never legitimately be that far behind.
class ReplayWindow {
 public:
  static constexpr std::size_t kBits = 1024;

  enum class Verdict { kFresh, kDuplicate, kStale };

  Verdict check_and_set(std::uint16_t seq);
  bool was_received(std::uint16_t seq) const;
  bool initialized() const { return initialized_; }
  std::uint16_t latest() const { return latest_; }

 private:
  bool bit(std::uint16_t seq) const {
    const std::size_t i = seq % kBits;
    return (bits_[i / 64] >> (i % 64)) & 1u;
  }
  void set_bit(std::uint16_t seq) {
    const std::size_t i = seq % kBits;
    bits_[i / 64] |= (1ull << (i % 64));
  }
  void clear_bit(std::uint16_t seq) {
    const std::size_t i = seq % kBits;
    bits_[i / 64] &= ~(1ull << (i % 64));
  }

  std::array<std::uint64_t, kBits / 64> bits_{};
  std::uint16_t latest_ = 0;
  bool initialized_ = false;
};

struct EndpointConfig {
  std::size_t max_datagram_bytes = kMaxDatagramBytes;
  // RFC 6298-flavored bounds, scaled for a LAN: clamp keeps one lost packet
  // from stalling an event for more than rto_max_s even with a wild estimate.
  double rto_initial_s = 0.2;
  double rto_min_s = 0.05;
  double rto_max_s = 1.0;
  // Max sit time for a pending ack before an ack-only packet goes out (acks
  // normally piggyback on regular traffic and cost nothing).
  double ack_delay_s = 0.025;
  // Oldest-unacked age at which the peer is declared dead (charter §6.3
  // philosophy: a peer that cannot confirm events for this long is gone).
  double peer_timeout_s = 10.0;
  std::size_t max_in_flight_messages = 256;
};

struct EndpointStats {
  std::uint64_t packets_sent = 0;
  std::uint64_t packets_received = 0;
  std::uint64_t packets_malformed = 0;
  std::uint64_t packets_duplicate = 0;
  std::uint64_t packets_stale = 0;
  std::uint64_t reliable_sent = 0;
  std::uint64_t retransmissions = 0;
  std::uint64_t messages_deduplicated = 0;
  std::uint64_t unreliable_dropped_oversize = 0;
};

class ReliableEndpoint {
 public:
  // (type, payload) of one decoded-envelope message. Payload excludes msg_id.
  using MessageHandler = std::function<void(MsgType, std::span<const std::uint8_t>)>;
  using DatagramSink = std::function<void(std::span<const std::uint8_t>)>;

  explicit ReliableEndpoint(EndpointConfig config = {});

  // Parses one incoming datagram, applies its acks, dedups, and delivers
  // fresh messages to handler. Returns false if the datagram was rejected
  // (bad magic/version, dup/stale packet, malformed framing). Messages
  // already delivered before a mid-packet framing error stay delivered.
  bool on_datagram(double now_s, std::span<const std::uint8_t> datagram,
                   const MessageHandler& handler);

  // Queues a fire-and-forget message for the next flush. Oversized payloads
  // (> one datagram) are dropped and counted: snapshot batching upstream is
  // responsible for staying under the budget.
  void send_unreliable(MsgType type, std::span<const std::uint8_t> payload);

  // Queues a Reliable-Unordered message. False = in-flight cap reached or
  // payload cannot fit one datagram — the caller should treat the connection
  // as failed (same eviction philosophy as the TCP send-queue cap).
  bool send_reliable(MsgType type, std::span<const std::uint8_t> payload);

  // Emits everything due at now_s as datagrams: fresh + RTO-expired reliable
  // messages first, then unreliable bundles, then a bare ack packet if acks
  // are due and nothing else carried them.
  void flush(double now_s, const DatagramSink& sink);

  bool has_rtt_sample() const { return srtt_s_ > 0.0; }
  double srtt_s() const { return srtt_s_; }
  double rto_s() const;
  std::size_t in_flight() const { return in_flight_.size(); }
  // True when the oldest in-flight message has gone unacknowledged for
  // peer_timeout_s — the reliable layer's liveness verdict on the peer.
  bool peer_timed_out(double now_s) const;
  const EndpointStats& stats() const { return stats_; }

 private:
  struct InFlightMessage {
    std::vector<std::uint8_t> envelope;  // [type][len][msg_id][payload]
    double first_sent_s = -1.0;          // <0 = never sent yet.
    double last_sent_s = -1.0;
  };

  struct SentPacketSlot {
    std::uint16_t seq = 0;
    bool used = false;
    bool acked = false;
    double sent_time_s = 0.0;
    std::vector<std::uint16_t> msg_ids;
  };

  void mark_acked(std::uint16_t seq, double now_s);
  void add_rtt_sample(double sample_s);
  void write_header(Writer& w, Channel channel, std::uint16_t seq) const;
  std::uint16_t record_sent_packet(double now_s, std::vector<std::uint16_t> msg_ids);
  std::size_t payload_budget() const { return config_.max_datagram_bytes - kPacketHeaderBytes; }

  EndpointConfig config_;
  EndpointStats stats_;

  // Send side.
  std::uint16_t next_seq_ = 0;
  std::uint16_t next_msg_id_ = 0;
  std::map<std::uint16_t, InFlightMessage> in_flight_;
  std::vector<std::vector<std::uint8_t>> pending_unreliable_;
  std::array<SentPacketSlot, ReplayWindow::kBits> sent_{};

  // Receive side.
  ReplayWindow packet_window_;
  ReplayWindow reliable_window_;
  bool ack_needed_ = false;
  double ack_deadline_s_ = 0.0;

  // RTT estimator (RFC 6298 shape).
  double srtt_s_ = 0.0;
  double rttvar_s_ = 0.0;
};

}  // namespace seashield::protocol
