#pragma once

#include <bit>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Binary wire primitives (charter §6): explicit little-endian packing with no
// implicit struct layout — every field is written and read one at a time, so
// the format is identical regardless of host padding or endianness.
//
// The protocol library depends on the standard library ONLY (no core/, no
// net/): it is the piece that gets linked into the UE5 client unchanged
// (charter §4.1 "프로토콜 코어의 공유").
namespace seashield::protocol {

// Append-only serializer. All multi-byte integers are little-endian; floats
// travel as their IEEE-754 bit patterns.
class Writer {
 public:
  void u8(std::uint8_t v) { buf_.push_back(v); }
  void u16(std::uint16_t v) {
    buf_.push_back(static_cast<std::uint8_t>(v));
    buf_.push_back(static_cast<std::uint8_t>(v >> 8));
  }
  void u32(std::uint32_t v) {
    u16(static_cast<std::uint16_t>(v));
    u16(static_cast<std::uint16_t>(v >> 16));
  }
  void u64(std::uint64_t v) {
    u32(static_cast<std::uint32_t>(v));
    u32(static_cast<std::uint32_t>(v >> 32));
  }
  void i16(std::int16_t v) { u16(static_cast<std::uint16_t>(v)); }
  // 24-bit two's complement (quantized positions, charter §6 "전송 규모 산정").
  // Precondition: v fits in [-2^23, 2^23-1]; the quantizers clamp before this.
  void i24(std::int32_t v) {
    const auto u = static_cast<std::uint32_t>(v);
    buf_.push_back(static_cast<std::uint8_t>(u));
    buf_.push_back(static_cast<std::uint8_t>(u >> 8));
    buf_.push_back(static_cast<std::uint8_t>(u >> 16));
  }
  void i32(std::int32_t v) { u32(static_cast<std::uint32_t>(v)); }
  void f32(float v) { u32(std::bit_cast<std::uint32_t>(v)); }
  void f64(double v) { u64(std::bit_cast<std::uint64_t>(v)); }
  void bytes(std::span<const std::uint8_t> data) { buf_.insert(buf_.end(), data.begin(), data.end()); }
  // u16 length prefix + raw bytes. Oversized input is truncated defensively;
  // message definitions cap their strings far below this.
  void str16(std::string_view s) {
    const std::size_t n = s.size() < 0xFFFF ? s.size() : 0xFFFF;
    u16(static_cast<std::uint16_t>(n));
    buf_.insert(buf_.end(), s.data(), s.data() + n);
  }

  const std::vector<std::uint8_t>& data() const { return buf_; }
  std::vector<std::uint8_t> take() { return std::move(buf_); }
  std::size_t size() const { return buf_.size(); }

 private:
  std::vector<std::uint8_t> buf_;
};

// Bounds-checked deserializer with sticky failure: any out-of-bounds read
// flips ok() to false and every subsequent read returns zero values. Callers
// run all reads first and check ok()/finished() once at the end — network
// input is untrusted, so nothing here ever reads past the buffer.
class Reader {
 public:
  explicit Reader(std::span<const std::uint8_t> data) : data_(data) {}

  std::uint8_t u8() {
    if (!take(1)) return 0;
    return data_[pos_++];
  }
  std::uint16_t u16() {
    if (!take(2)) return 0;
    const auto v = static_cast<std::uint16_t>(data_[pos_] | (data_[pos_ + 1] << 8));
    pos_ += 2;
    return v;
  }
  // Multi-byte reads are atomic: on a short buffer they consume nothing and
  // return 0, never a partially assembled value.
  std::uint32_t u32() {
    if (!take(4)) return 0;
    std::uint32_t v = 0;
    for (int i = 3; i >= 0; --i) {
      v = (v << 8) | data_[pos_ + static_cast<std::size_t>(i)];
    }
    pos_ += 4;
    return v;
  }
  std::uint64_t u64() {
    if (!take(8)) return 0;
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i) {
      v = (v << 8) | data_[pos_ + static_cast<std::size_t>(i)];
    }
    pos_ += 8;
    return v;
  }
  std::int16_t i16() { return static_cast<std::int16_t>(u16()); }
  std::int32_t i24() {
    if (!take(3)) return 0;
    std::uint32_t u = static_cast<std::uint32_t>(data_[pos_]) |
                      (static_cast<std::uint32_t>(data_[pos_ + 1]) << 8) |
                      (static_cast<std::uint32_t>(data_[pos_ + 2]) << 16);
    pos_ += 3;
    if (u & 0x800000u) {
      u |= 0xFF000000u;  // Sign-extend bit 23.
    }
    return static_cast<std::int32_t>(u);
  }
  std::int32_t i32() { return static_cast<std::int32_t>(u32()); }
  float f32() { return std::bit_cast<float>(u32()); }
  double f64() { return std::bit_cast<double>(u64()); }
  std::span<const std::uint8_t> bytes(std::size_t n) {
    if (!take(n)) return {};
    const auto view = data_.subspan(pos_, n);
    pos_ += n;
    return view;
  }
  std::string str16() {
    const std::uint16_t n = u16();
    const auto view = bytes(n);
    return std::string(view.begin(), view.end());
  }

  bool ok() const { return ok_; }
  std::size_t remaining() const { return data_.size() - pos_; }
  // Decoders require exact consumption: trailing garbage is a malformed
  // message, same strictness as the scenario parser (charter culture).
  bool finished() const { return ok_ && pos_ == data_.size(); }

 private:
  bool take(std::size_t n) {
    if (!ok_ || data_.size() - pos_ < n) {
      ok_ = false;
      return false;
    }
    return true;
  }

  std::span<const std::uint8_t> data_;
  std::size_t pos_ = 0;
  bool ok_ = true;
};

}  // namespace seashield::protocol
