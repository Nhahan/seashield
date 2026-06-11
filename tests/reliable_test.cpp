#include "protocol/reliable.h"

#include <cstdint>
#include <map>
#include <set>
#include <vector>

#include <gtest/gtest.h>

#include "core/pcg32.h"
#include "protocol/messages.h"
#include "protocol/wire.h"

// P3a gate (charter §9, risk table): the sequence/ack/retransmission logic is
// proven against deterministic loss/duplication/reordering injection BEFORE
// anything is built on top of the channel. Time is virtual throughout — every
// run is bit-reproducible from its seed.
namespace seashield::protocol {
namespace {

std::vector<std::uint8_t> index_payload(std::uint32_t index) {
  Writer w;
  w.u32(index);
  return w.take();
}

std::uint32_t payload_index(std::span<const std::uint8_t> payload) {
  Reader r(payload);
  return r.u32();
}

// Deterministic chaos link: per-direction in-flight queues keyed by virtual
// delivery time. Jitter makes delivery times non-monotonic, which is exactly
// how real reordering happens.
class ChaosLink {
 public:
  struct Config {
    double loss = 0.0;
    double dup = 0.0;
    double delay_s = 0.02;
    double jitter_s = 0.0;
  };

  ChaosLink(Config config, std::uint64_t seed) : config_(config), rng_(seed) {}

  void send(std::multimap<double, std::vector<std::uint8_t>>& queue, double now,
            std::span<const std::uint8_t> bytes) {
    if (rng_.next_double() < config_.loss) {
      return;
    }
    queue.emplace(now + config_.delay_s + rng_.uniform(0.0, config_.jitter_s),
                  std::vector<std::uint8_t>(bytes.begin(), bytes.end()));
    if (rng_.next_double() < config_.dup) {
      queue.emplace(now + config_.delay_s + rng_.uniform(0.0, config_.jitter_s),
                    std::vector<std::uint8_t>(bytes.begin(), bytes.end()));
    }
  }

  void deliver_due(std::multimap<double, std::vector<std::uint8_t>>& queue, double now,
                   ReliableEndpoint& target, const ReliableEndpoint::MessageHandler& handler) {
    while (!queue.empty() && queue.begin()->first <= now) {
      target.on_datagram(now, queue.begin()->second, handler);
      queue.erase(queue.begin());
    }
  }

  std::multimap<double, std::vector<std::uint8_t>> to_a, to_b;

 private:
  Config config_;
  Pcg32 rng_;
};

// Two endpoints joined by a chaos link, stepped on a virtual clock.
struct LinkedPair {
  explicit LinkedPair(ChaosLink::Config link_config, std::uint64_t seed,
                      EndpointConfig endpoint_config = {})
      : link(link_config, seed), a(endpoint_config), b(endpoint_config) {
    a_handler = [this](MsgType type, std::span<const std::uint8_t> payload) {
      a_received.emplace_back(type, payload_index(payload));
    };
    b_handler = [this](MsgType type, std::span<const std::uint8_t> payload) {
      b_received.emplace_back(type, payload_index(payload));
    };
  }

  void step(double dt = 0.016) {
    now += dt;
    a.flush(now, [&](std::span<const std::uint8_t> d) { link.send(link.to_b, now, d); });
    b.flush(now, [&](std::span<const std::uint8_t> d) { link.send(link.to_a, now, d); });
    link.deliver_due(link.to_b, now, b, b_handler);
    link.deliver_due(link.to_a, now, a, a_handler);
  }

  ChaosLink link;
  ReliableEndpoint a, b;
  ReliableEndpoint::MessageHandler a_handler, b_handler;
  std::vector<std::pair<MsgType, std::uint32_t>> a_received, b_received;
  double now = 0.0;
};

TEST(ReliableTest, SequenceComparisonHandlesWraparound) {
  EXPECT_TRUE(seq_newer(1, 0));
  EXPECT_FALSE(seq_newer(0, 1));
  EXPECT_FALSE(seq_newer(5, 5));
  EXPECT_TRUE(seq_newer(0, 65535));     // Wrap: 0 follows 65535.
  EXPECT_TRUE(seq_newer(100, 65500));   // Wrap across the seam.
  EXPECT_FALSE(seq_newer(65500, 100));
  EXPECT_EQ(seq_diff(0, 65535), 1);
  EXPECT_EQ(seq_diff(65535, 0), -1);
}

TEST(ReliableTest, ReplayWindowVerdicts) {
  ReplayWindow win;
  EXPECT_EQ(win.check_and_set(10), ReplayWindow::Verdict::kFresh);
  EXPECT_EQ(win.check_and_set(10), ReplayWindow::Verdict::kDuplicate);
  EXPECT_EQ(win.check_and_set(12), ReplayWindow::Verdict::kFresh);
  EXPECT_EQ(win.check_and_set(11), ReplayWindow::Verdict::kFresh);  // Reordered, in window.
  EXPECT_EQ(win.check_and_set(11), ReplayWindow::Verdict::kDuplicate);
  EXPECT_TRUE(win.was_received(10));
  EXPECT_FALSE(win.was_received(9));

  // Advance far enough that old state must be forgotten, then verify the
  // recycled bit slots read as "not received" (no false acks).
  EXPECT_EQ(win.check_and_set(static_cast<std::uint16_t>(12 + 2000)), ReplayWindow::Verdict::kFresh);
  EXPECT_FALSE(win.was_received(11));
  EXPECT_EQ(win.check_and_set(11), ReplayWindow::Verdict::kStale);

  // Wraparound: window keeps working across the 16-bit seam.
  ReplayWindow wrap;
  EXPECT_EQ(wrap.check_and_set(65534), ReplayWindow::Verdict::kFresh);
  EXPECT_EQ(wrap.check_and_set(2), ReplayWindow::Verdict::kFresh);
  EXPECT_EQ(wrap.check_and_set(65535), ReplayWindow::Verdict::kFresh);
  EXPECT_EQ(wrap.check_and_set(65535), ReplayWindow::Verdict::kDuplicate);
  EXPECT_TRUE(wrap.was_received(65534));
  EXPECT_FALSE(wrap.was_received(0));
}

TEST(ReliableTest, ReplayWindowExactWindowBoundary) {
  // Advance by exactly kBits: every recycled bit slot must read "not
  // received" — an off-by-one here would fake acks or break dedup.
  ReplayWindow win;
  EXPECT_EQ(win.check_and_set(0), ReplayWindow::Verdict::kFresh);
  EXPECT_EQ(win.check_and_set(static_cast<std::uint16_t>(ReplayWindow::kBits)),
            ReplayWindow::Verdict::kFresh);
  EXPECT_FALSE(win.was_received(0));
  EXPECT_EQ(win.check_and_set(0), ReplayWindow::Verdict::kStale);

  // One inside the edge: the oldest in-window sequence is still remembered.
  ReplayWindow edge;
  EXPECT_EQ(edge.check_and_set(0), ReplayWindow::Verdict::kFresh);
  EXPECT_EQ(edge.check_and_set(static_cast<std::uint16_t>(ReplayWindow::kBits - 1)),
            ReplayWindow::Verdict::kFresh);
  EXPECT_TRUE(edge.was_received(0));
  EXPECT_EQ(edge.check_and_set(0), ReplayWindow::Verdict::kDuplicate);
}

// Locks the packet header byte layout to the spec: a third-party
// implementation reading protocol-spec.md must interoperate.
TEST(ReliableTest, HeaderAcksReflectReceiveWindowWithAGap) {
  ReliableEndpoint a, b;
  auto sink_collect = [](std::vector<std::vector<std::uint8_t>>& out) {
    return [&out](std::span<const std::uint8_t> d) {
      out.emplace_back(d.begin(), d.end());
    };
  };

  // Five packets seq 0..4, one reliable message each (flushing in between so
  // each message rides its own packet).
  std::vector<std::vector<std::uint8_t>> packets;
  for (std::uint32_t i = 0; i < 5; ++i) {
    ASSERT_TRUE(a.send_reliable(MsgType::kEngagementEvent, index_payload(i)));
    a.flush(0.001 * (i + 1), sink_collect(packets));
  }
  ASSERT_EQ(packets.size(), 5u);

  // Deliver 0,1,2,4 — packet 3 is lost.
  int delivered = 0;
  const auto count = [&](MsgType, std::span<const std::uint8_t>) { ++delivered; };
  for (const std::size_t idx : {0u, 1u, 2u, 4u}) {
    EXPECT_TRUE(b.on_datagram(0.01, packets[idx], count));
  }
  EXPECT_EQ(delivered, 4);

  // B owes an ack; past the ack delay it emits an ack-only packet.
  std::vector<std::vector<std::uint8_t>> acks;
  b.flush(0.05, sink_collect(acks));
  ASSERT_EQ(acks.size(), 1u);
  const auto& h = acks[0];
  ASSERT_EQ(h.size(), kPacketHeaderBytes);
  EXPECT_EQ(h[0], 0xA5);  // magic 0x5EA5, little-endian.
  EXPECT_EQ(h[1], 0x5E);
  EXPECT_EQ(h[2], kProtocolVersion);
  // B has received packets, so bit 7 (ack-valid) is set on the channel byte.
  EXPECT_EQ(h[3], static_cast<std::uint8_t>(Channel::kUnreliable) | kAckValidFlag);
  EXPECT_EQ(h[6], 4);  // ack = latest received seq (4).
  EXPECT_EQ(h[7], 0);
  // ack_bits bit i = seq (4-1-i): bit0=seq3 lost, bit1=seq2, bit2=seq1, bit3=seq0.
  EXPECT_EQ(h[8], 0b00001110);
  EXPECT_EQ(h[9], 0);
  EXPECT_EQ(h[10], 0);
  EXPECT_EQ(h[11], 0);

  // The ack clears everything it covers from A's in-flight set; the lost
  // packet's message (index 3) stays pending for retransmission.
  a.on_datagram(0.06, acks[0], count);
  EXPECT_EQ(a.in_flight(), 1u);
}

TEST(ReliableTest, PerfectLinkDeliversExactlyOnceWithoutRetransmissions) {
  LinkedPair pair({.loss = 0.0, .dup = 0.0, .delay_s = 0.005}, 7);
  for (std::uint32_t i = 0; i < 50; ++i) {
    ASSERT_TRUE(pair.a.send_reliable(MsgType::kEngagementEvent, index_payload(i)));
  }
  for (int step = 0; step < 60 && (pair.b_received.size() < 50 || pair.a.in_flight() > 0);
       ++step) {
    pair.step();
  }
  ASSERT_EQ(pair.b_received.size(), 50u);
  std::set<std::uint32_t> indices;
  for (const auto& [type, index] : pair.b_received) {
    EXPECT_EQ(type, MsgType::kEngagementEvent);
    EXPECT_TRUE(indices.insert(index).second) << "duplicate delivery of " << index;
  }
  EXPECT_EQ(pair.a.in_flight(), 0u);
  EXPECT_EQ(pair.a.stats().retransmissions, 0u);
  EXPECT_EQ(pair.b.stats().messages_deduplicated, 0u);
}

TEST(ReliableTest, LostFirstTransmissionIsRetransmittedAfterRto) {
  ReliableEndpoint a, b;
  ASSERT_TRUE(a.send_reliable(MsgType::kEngagementEvent, index_payload(99)));

  // First flush: the datagram evaporates (loss).
  int first_flush_packets = 0;
  a.flush(0.0, [&](std::span<const std::uint8_t>) { ++first_flush_packets; });
  EXPECT_EQ(first_flush_packets, 1);
  EXPECT_EQ(a.stats().retransmissions, 0u);

  // Before the RTO nothing happens; after it the message rides a new packet.
  int packets = 0;
  a.flush(0.1, [&](std::span<const std::uint8_t>) { ++packets; });
  EXPECT_EQ(packets, 0) << "retransmitted before RTO expired";
  std::vector<std::uint8_t> retransmit;
  a.flush(0.25, [&](std::span<const std::uint8_t> d) {
    ++packets;
    retransmit.assign(d.begin(), d.end());
  });
  ASSERT_EQ(packets, 1);
  EXPECT_EQ(a.stats().retransmissions, 1u);

  std::uint32_t delivered_index = 0;
  int delivered = 0;
  EXPECT_TRUE(b.on_datagram(0.26, retransmit, [&](MsgType, std::span<const std::uint8_t> p) {
    ++delivered;
    delivered_index = payload_index(p);
  }));
  EXPECT_EQ(delivered, 1);
  EXPECT_EQ(delivered_index, 99u);
}

TEST(ReliableTest, DuplicateDatagramIsDroppedWhole) {
  ReliableEndpoint a, b;
  ASSERT_TRUE(a.send_reliable(MsgType::kEngagementEvent, index_payload(1)));
  std::vector<std::uint8_t> datagram;
  a.flush(0.0, [&](std::span<const std::uint8_t> d) { datagram.assign(d.begin(), d.end()); });

  int delivered = 0;
  const auto count = [&](MsgType, std::span<const std::uint8_t>) { ++delivered; };
  EXPECT_TRUE(b.on_datagram(0.01, datagram, count));
  EXPECT_FALSE(b.on_datagram(0.02, datagram, count));
  EXPECT_EQ(delivered, 1);
  EXPECT_EQ(b.stats().packets_duplicate, 1u);
}

// The gate test (charter risk table): loss × duplication × reordering sweep,
// every combination seeded and reproducible. Reliable messages must arrive
// exactly once and the sender's in-flight set must drain.
TEST(ReliableTest, ChaosSweepDeliversReliableExactlyOnce) {
  const double losses[] = {0.0, 0.05, 0.2, 0.35};
  const double dups[] = {0.0, 0.1};
  const double jitters[] = {0.0, 0.08};
  const std::uint64_t seeds[] = {1, 2, 3};
  constexpr std::uint32_t kMessages = 200;

  for (const double loss : losses) {
    for (const double dup : dups) {
      for (const double jitter : jitters) {
        for (const std::uint64_t seed : seeds) {
          LinkedPair pair({.loss = loss, .dup = dup, .delay_s = 0.02, .jitter_s = jitter}, seed);
          std::uint32_t sent = 0;
          std::uint32_t unreliable_sent = 0;
          // 25s virtual budget; with RTO ≤ 1s even 35% loss converges well
          // inside it. Mixed traffic: B streams unreliable messages the
          // whole time so acks ride realistic packets.
          for (int step = 0; step < 1500; ++step) {
            for (int burst = 0; burst < 4 && sent < kMessages; ++burst) {
              ASSERT_TRUE(pair.a.send_reliable(MsgType::kEngagementEvent, index_payload(sent)));
              ++sent;
            }
            pair.b.send_unreliable(MsgType::kKeepalive, index_payload(unreliable_sent++));
            pair.step();
            if (sent == kMessages && pair.b_received.size() >= kMessages &&
                pair.a.in_flight() == 0) {
              break;
            }
          }

          const std::string ctx = "loss=" + std::to_string(loss) + " dup=" + std::to_string(dup) +
                                  " jitter=" + std::to_string(jitter) +
                                  " seed=" + std::to_string(seed);
          std::set<std::uint32_t> indices;
          for (const auto& [type, index] : pair.b_received) {
            EXPECT_EQ(type, MsgType::kEngagementEvent) << ctx;
            EXPECT_TRUE(indices.insert(index).second) << ctx << " duplicate index " << index;
          }
          EXPECT_EQ(indices.size(), kMessages) << ctx;
          EXPECT_EQ(pair.a.in_flight(), 0u) << ctx;

          // Unreliable deliveries on A's side: anything that arrived must be
          // uncorrupted (a valid index that B actually sent), but nothing is
          // promised about completeness.
          for (const auto& [type, index] : pair.a_received) {
            EXPECT_EQ(type, MsgType::kKeepalive) << ctx;
            EXPECT_LT(index, unreliable_sent) << ctx;
          }
        }
      }
    }
  }
}

TEST(ReliableTest, MessageIdWindowSurvivesWraparound) {
  // 70k messages cross the 16-bit msg_id seam; ids recycle but the sliding
  // dedup window must keep every delivery unique.
  LinkedPair pair({.loss = 0.0, .dup = 0.0, .delay_s = 0.001}, 11);
  constexpr std::uint32_t kTotal = 70000;
  std::uint32_t sent = 0;
  std::uint64_t delivered = 0;
  std::vector<bool> seen(kTotal, false);
  bool exactly_once = true;
  // Override handler: storing 70k pairs is wasteful — verify on the fly.
  // No order assertion: the channel is Reliable-UNORDERED by contract, and
  // msg_id wraparound genuinely reorders packing at the 65536 seam.
  pair.b_handler = [&](MsgType, std::span<const std::uint8_t> payload) {
    ++delivered;
    const std::uint32_t index = payload_index(payload);
    if (index >= kTotal || seen[index]) {
      exactly_once = false;
    } else {
      seen[index] = true;
    }
  };
  while (sent < kTotal || pair.a.in_flight() > 0) {
    // Stay under the in-flight cap (256 default): acks lag a round trip, so
    // unchecked bursts would overflow it.
    for (int burst = 0; burst < 100 && sent < kTotal && pair.a.in_flight() < 150; ++burst) {
      ASSERT_TRUE(pair.a.send_reliable(MsgType::kEngagementEvent, index_payload(sent)));
      ++sent;
    }
    pair.step(0.016);
    ASSERT_LT(pair.now, 60.0) << "wrap test failed to converge";
  }
  EXPECT_EQ(delivered, kTotal);
  EXPECT_TRUE(exactly_once);
  EXPECT_EQ(pair.a.stats().retransmissions, 0u);
}

TEST(ReliableTest, RttEstimatorTracksLinkDelay) {
  // Symmetric 40ms link: RTT sample = 80ms + ack delay (≤25ms + step grid).
  LinkedPair pair({.loss = 0.0, .dup = 0.0, .delay_s = 0.04}, 3);
  for (int step = 0; step < 120; ++step) {
    if (step % 2 == 0) {
      ASSERT_TRUE(
          pair.a.send_reliable(MsgType::kEngagementEvent, index_payload(unsigned(step))));
    }
    pair.step();
  }
  ASSERT_TRUE(pair.a.has_rtt_sample());
  EXPECT_GT(pair.a.srtt_s(), 0.07);
  EXPECT_LT(pair.a.srtt_s(), 0.16);
  EXPECT_GE(pair.a.rto_s(), pair.a.srtt_s());
  EXPECT_LE(pair.a.rto_s(), 1.0);
}

TEST(ReliableTest, GarbageAndTruncatedDatagramsAreHarmless) {
  ReliableEndpoint endpoint;
  int delivered = 0;
  const auto handler = [&](MsgType, std::span<const std::uint8_t>) { ++delivered; };

  Pcg32 rng(0xFEED);
  std::vector<std::uint8_t> noise;
  for (int i = 0; i < 2000; ++i) {
    noise.resize(rng.next() % 1400);
    for (auto& byte : noise) {
      byte = static_cast<std::uint8_t>(rng.next());
    }
    endpoint.on_datagram(0.001 * i, noise, handler);  // Must not crash or deliver junk acks.
  }

  // Truncate a real packet at every possible length: parser must reject all
  // prefixes without crashing, and at most deliver already-complete messages.
  ReliableEndpoint sender;
  ASSERT_TRUE(sender.send_reliable(MsgType::kEngagementEvent, index_payload(5)));
  std::vector<std::uint8_t> packet;
  sender.flush(0.0, [&](std::span<const std::uint8_t> d) { packet.assign(d.begin(), d.end()); });
  for (std::size_t len = 0; len < packet.size(); ++len) {
    ReliableEndpoint fresh;
    fresh.on_datagram(0.0, std::span(packet.data(), len), handler);
  }

  // A pristine endpoint still works after the noise barrage.
  ReliableEndpoint receiver;
  delivered = 0;
  EXPECT_TRUE(receiver.on_datagram(3.0, packet, handler));
  EXPECT_EQ(delivered, 1);
}

TEST(ReliableTest, InFlightCapRejectsFurtherReliableSends) {
  EndpointConfig config;
  config.max_in_flight_messages = 8;
  ReliableEndpoint endpoint(config);
  for (std::uint32_t i = 0; i < 8; ++i) {
    EXPECT_TRUE(endpoint.send_reliable(MsgType::kEngagementEvent, index_payload(i)));
  }
  EXPECT_FALSE(endpoint.send_reliable(MsgType::kEngagementEvent, index_payload(8)));
  EXPECT_EQ(endpoint.in_flight(), 8u);
}

TEST(ReliableTest, PeerTimeoutFiresOnlyAfterProlongedSilence) {
  ReliableEndpoint endpoint;
  ASSERT_TRUE(endpoint.send_reliable(MsgType::kEngagementEvent, index_payload(0)));
  endpoint.flush(0.0, [](std::span<const std::uint8_t>) {});  // Into the void.
  EXPECT_FALSE(endpoint.peer_timed_out(5.0));
  EXPECT_TRUE(endpoint.peer_timed_out(10.5));
}

TEST(ReliableTest, UnreliableTrafficNeverTriggersAckOnlyPackets) {
  ReliableEndpoint a, b;
  a.send_unreliable(MsgType::kKeepalive, index_payload(0));
  std::vector<std::uint8_t> datagram;
  a.flush(0.0, [&](std::span<const std::uint8_t> d) { datagram.assign(d.begin(), d.end()); });
  int delivered = 0;
  EXPECT_TRUE(b.on_datagram(0.01, datagram,
                            [&](MsgType, std::span<const std::uint8_t>) { ++delivered; }));
  EXPECT_EQ(delivered, 1);
  // Long after any ack delay, B still has nothing to say: no ack-ack loop.
  int packets = 0;
  b.flush(1.0, [&](std::span<const std::uint8_t>) { ++packets; });
  EXPECT_EQ(packets, 0);
}

TEST(ReliableTest, UnreliableMessagesBundleUpToTheDatagramBudget) {
  ReliableEndpoint a, b;
  const std::vector<std::uint8_t> big(400, 0xAB);
  for (int i = 0; i < 3; ++i) {
    a.send_unreliable(MsgType::kSnapshot, big);  // 3 × 403B + 12B > 1200B.
  }
  int packets = 0;
  int delivered = 0;
  std::vector<std::vector<std::uint8_t>> datagrams;
  a.flush(0.0, [&](std::span<const std::uint8_t> d) {
    ++packets;
    EXPECT_LE(d.size(), kMaxDatagramBytes);
    datagrams.emplace_back(d.begin(), d.end());
  });
  EXPECT_EQ(packets, 2);
  for (const auto& d : datagrams) {
    EXPECT_TRUE(b.on_datagram(0.01, d, [&](MsgType type, std::span<const std::uint8_t> payload) {
      EXPECT_EQ(type, MsgType::kSnapshot);
      EXPECT_EQ(payload.size(), big.size());
      ++delivered;
    }));
  }
  EXPECT_EQ(delivered, 3);
}

TEST(ReliableTest, OversizedMessagesAreRefused) {
  ReliableEndpoint endpoint;
  const std::vector<std::uint8_t> huge(kMaxDatagramBytes, 0x00);
  endpoint.send_unreliable(MsgType::kSnapshot, huge);
  EXPECT_EQ(endpoint.stats().unreliable_dropped_oversize, 1u);
  EXPECT_FALSE(endpoint.send_reliable(MsgType::kEngagementEvent, huge));
  int packets = 0;
  endpoint.flush(0.0, [&](std::span<const std::uint8_t>) { ++packets; });
  EXPECT_EQ(packets, 0);
}

}  // namespace
}  // namespace seashield::protocol
