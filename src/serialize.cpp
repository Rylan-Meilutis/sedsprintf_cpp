#include "internal.hpp"

#include <algorithm>
#include <cstring>

#ifdef SEDS_HAS_ZSTD
#include <zstd.h>
#endif

namespace seds {
namespace {

size_t endpoint_bitmap_bytes() { return static_cast<size_t>((kEndpointCount + 7u) / 8u); }

} // namespace

void write_uleb128(uint64_t value, std::vector<uint8_t> &out) {
  do {
    auto byte = static_cast<uint8_t>(value & 0x7F);
    value >>= 7u;
    if (value != 0) {
      byte |= 0x80u;
    }
    out.push_back(byte);
  } while (value != 0);
}

bool read_uleb128(const uint8_t *&cur, const uint8_t *end, uint64_t &out) {
  out = 0;
  int shift = 0;
  for (int i = 0; i < 10; ++i) {
    if (cur >= end) {
      return false;
    }
    const uint8_t byte = *cur++;
    out |= static_cast<uint64_t>(byte & 0x7F) << shift;
    if ((byte & 0x80u) == 0) {
      return true;
    }
    shift += 7;
  }
  return false;
}

std::vector<uint8_t> endpoint_bitmap(const std::vector<uint32_t> &endpoints) {
  std::vector<uint8_t> bitmap(endpoint_bitmap_bytes(), 0);
  for (const uint32_t ep : endpoints) {
    if (valid_endpoint(ep)) {
      bitmap[ep / 8u] |= static_cast<uint8_t>(1u << (ep % 8u));
    }
  }
  return bitmap;
}

std::vector<uint32_t> parse_bitmap(const uint8_t *bitmap, size_t len) {
  std::vector<uint32_t> out;
  const size_t max_bits = std::min<size_t>(len * 8u, kEndpointCount);
  for (size_t i = 0; i < max_bits; ++i) {
    if ((bitmap[i / 8] & (1u << (i % 8))) != 0u) {
      out.push_back(static_cast<uint32_t>(i));
    }
  }
  return out;
}

std::vector<uint8_t> maybe_compress(const uint8_t *data, size_t len, bool &compressed) {
  compressed = false;
#ifdef SEDS_HAS_ZSTD
  if (len >= kCompressionThreshold) {
    std::vector<uint8_t> out(ZSTD_compressBound(len));
    const size_t got = ZSTD_compress(out.data(), out.size(), data, len, 1);
    if (!ZSTD_isError(got) && got < len) {
      out.resize(got);
      compressed = true;
      return out;
    }
  }
#endif
  return {data, data + len};
}

std::vector<uint8_t> maybe_decompress(const uint8_t *data, size_t wire_len, size_t logical_len, bool compressed) {
  if (!compressed) {
    return {data, data + wire_len};
  }
#ifdef SEDS_HAS_ZSTD
  std::vector<uint8_t> out(logical_len);
  const size_t got = ZSTD_decompress(out.data(), out.size(), data, wire_len);
  if (ZSTD_isError(got) || got != logical_len) {
    return {};
  }
  return out;
#else
  (void)logical_len;
  return {};
#endif
}

std::optional<PacketData> deserialize_packet(const uint8_t *bytes, size_t len) {
  if (bytes == nullptr || len < 2 + kCrcBytes) {
    return std::nullopt;
  }
  const uint32_t got_crc = crc32_bytes(bytes, len - kCrcBytes);
  uint32_t exp_crc = 0;
  std::memcpy(&exp_crc, bytes + len - kCrcBytes, kCrcBytes);
  if (got_crc != exp_crc) {
    return std::nullopt;
  }

  const uint8_t *cur = bytes;
  const uint8_t *end = bytes + len - kCrcBytes;
  const uint8_t flags = *cur++;
  const uint8_t nep = *cur++;
  uint64_t ty = 0;
  uint64_t logical_size = 0;
  uint64_t timestamp = 0;
  uint64_t sender_len = 0;
  uint64_t sender_wire_len = 0;
  if (!read_uleb128(cur, end, ty) || !read_uleb128(cur, end, logical_size) || !read_uleb128(cur, end, timestamp) ||
      !read_uleb128(cur, end, sender_len)) {
    return std::nullopt;
  }
  if ((flags & kFlagCompressedSender) != 0u) {
    if (!read_uleb128(cur, end, sender_wire_len)) {
      return std::nullopt;
    }
  } else {
    sender_wire_len = sender_len;
  }
  if (!valid_type(static_cast<uint32_t>(ty))) {
    return std::nullopt;
  }
  const size_t bitmap_len = endpoint_bitmap_bytes();
  if (static_cast<size_t>(end - cur) < bitmap_len + sender_wire_len) {
    return std::nullopt;
  }
  std::vector<uint32_t> endpoints = parse_bitmap(cur, bitmap_len);
  cur += bitmap_len;
  if (endpoints.size() != nep) {
    return std::nullopt;
  }

  const auto sender_bytes = maybe_decompress(cur, static_cast<size_t>(sender_wire_len), static_cast<size_t>(sender_len),
                                             (flags & kFlagCompressedSender) != 0u);
  if (sender_bytes.size() != sender_len) {
    return std::nullopt;
  }
  cur += sender_wire_len;
  if (kTypeInfo[ty].reliable()) {
    if (static_cast<size_t>(end - cur) < kReliableHeaderBytes) {
      return std::nullopt;
    }
    if ((cur[0] & 0x01u) != 0u) {
      return std::nullopt;
    }
    cur += kReliableHeaderBytes;
  }

  const auto payload_wire_len = static_cast<size_t>(end - cur);
  auto payload = maybe_decompress(cur, payload_wire_len, static_cast<size_t>(logical_size),
                                  (flags & kFlagCompressedPayload) != 0u);
  if (payload.size() != logical_size) {
    return std::nullopt;
  }

  PacketData pkt;
  pkt.ty = static_cast<uint32_t>(ty);
  pkt.sender.assign(reinterpret_cast<const char *>(sender_bytes.data()), sender_bytes.size());
  pkt.endpoints = std::move(endpoints);
  pkt.timestamp = timestamp;
  pkt.payload = std::move(payload);
  return pkt;
}

std::vector<uint8_t> serialize_packet(const PacketData &pkt) {
  if (kTypeInfo[pkt.ty].reliable()) {
    return serialize_packet_with_reliable(pkt, ReliableHeaderLite{0x80u, 0u, 0u});
  }
  std::vector<uint8_t> out;
  bool sender_compressed = false;
  bool payload_compressed = false;
  auto sender_wire =
      maybe_compress(reinterpret_cast<const uint8_t *>(pkt.sender.data()), pkt.sender.size(), sender_compressed);
  auto payload_wire = maybe_compress(pkt.payload.data(), pkt.payload.size(), payload_compressed);
  uint8_t flags = 0;
  if (payload_compressed)
    flags |= kFlagCompressedPayload;
  if (sender_compressed)
    flags |= kFlagCompressedSender;
  out.push_back(flags);
  out.push_back(static_cast<uint8_t>(pkt.endpoints.size()));
  write_uleb128(pkt.ty, out);
  write_uleb128(pkt.payload.size(), out);
  write_uleb128(pkt.timestamp, out);
  write_uleb128(pkt.sender.size(), out);
  if (sender_compressed) {
    write_uleb128(sender_wire.size(), out);
  }
  const auto bitmap = endpoint_bitmap(pkt.endpoints);
  out.insert(out.end(), bitmap.begin(), bitmap.end());
  out.insert(out.end(), sender_wire.begin(), sender_wire.end());
  out.insert(out.end(), payload_wire.begin(), payload_wire.end());
  append_le<uint32_t>(crc32_bytes(out.data(), out.size()), out);
  return out;
}

std::vector<uint8_t> serialize_packet_with_reliable(const PacketData &pkt, ReliableHeaderLite header) {
  std::vector<uint8_t> out;
  bool sender_compressed = false;
  bool payload_compressed = false;
  auto sender_wire =
      maybe_compress(reinterpret_cast<const uint8_t *>(pkt.sender.data()), pkt.sender.size(), sender_compressed);
  auto payload_wire = maybe_compress(pkt.payload.data(), pkt.payload.size(), payload_compressed);
  uint8_t flags = 0;
  if (payload_compressed)
    flags |= kFlagCompressedPayload;
  if (sender_compressed)
    flags |= kFlagCompressedSender;
  out.push_back(flags);
  out.push_back(static_cast<uint8_t>(pkt.endpoints.size()));
  write_uleb128(pkt.ty, out);
  write_uleb128(pkt.payload.size(), out);
  write_uleb128(pkt.timestamp, out);
  write_uleb128(pkt.sender.size(), out);
  if (sender_compressed) {
    write_uleb128(sender_wire.size(), out);
  }
  const auto bitmap = endpoint_bitmap(pkt.endpoints);
  out.insert(out.end(), bitmap.begin(), bitmap.end());
  out.insert(out.end(), sender_wire.begin(), sender_wire.end());
  if (kTypeInfo[pkt.ty].reliable()) {
    out.push_back(header.flags);
    append_le<uint32_t>(header.seq, out);
    append_le<uint32_t>(header.ack, out);
  }
  out.insert(out.end(), payload_wire.begin(), payload_wire.end());
  append_le<uint32_t>(crc32_bytes(out.data(), out.size()), out);
  return out;
}

std::vector<uint8_t> serialize_reliable_ack(std::string_view sender, uint32_t ty, uint64_t timestamp_ms, uint32_t ack) {
  std::vector<uint8_t> out;
  bool sender_compressed = false;
  auto sender_wire = maybe_compress(reinterpret_cast<const uint8_t *>(sender.data()), sender.size(), sender_compressed);
  uint8_t flags = 0;
  if (sender_compressed)
    flags |= kFlagCompressedSender;
  out.push_back(flags);
  out.push_back(0);
  write_uleb128(ty, out);
  write_uleb128(0, out);
  write_uleb128(timestamp_ms, out);
  write_uleb128(sender.size(), out);
  if (sender_compressed) {
    write_uleb128(sender_wire.size(), out);
  }
  out.resize(out.size() + endpoint_bitmap_bytes(), 0);
  out.insert(out.end(), sender_wire.begin(), sender_wire.end());
  out.push_back(0x01u);
  append_le<uint32_t>(0u, out);
  append_le<uint32_t>(ack, out);
  append_le<uint32_t>(crc32_bytes(out.data(), out.size()), out);
  return out;
}

std::optional<FrameInfoLite> peek_frame_info(const uint8_t *bytes, size_t len, bool verify_crc) {
  if (bytes == nullptr || len < 2 + kCrcBytes) {
    return std::nullopt;
  }
  if (verify_crc) {
    const uint32_t got_crc = crc32_bytes(bytes, len - kCrcBytes);
    uint32_t exp_crc = 0;
    std::memcpy(&exp_crc, bytes + len - kCrcBytes, kCrcBytes);
    if (got_crc != exp_crc) {
      return std::nullopt;
    }
  }
  const uint8_t *cur = bytes;
  const uint8_t *end = bytes + len - kCrcBytes;
  const uint8_t flags = *cur++;
  const bool sender_compressed = (flags & kFlagCompressedSender) != 0u;
  const uint8_t nep = *cur++;
  uint64_t ty = 0, dsz = 0, ts = 0, sender_len = 0, sender_wire_len = 0;
  if (!read_uleb128(cur, end, ty) || !read_uleb128(cur, end, dsz) || !read_uleb128(cur, end, ts) ||
      !read_uleb128(cur, end, sender_len)) {
    return std::nullopt;
  }
  if (sender_compressed) {
    if (!read_uleb128(cur, end, sender_wire_len)) {
      return std::nullopt;
    }
  } else {
    sender_wire_len = sender_len;
  }
  const size_t bitmap_len = endpoint_bitmap_bytes();
  if (!valid_type(static_cast<uint32_t>(ty)) || static_cast<size_t>(end - cur) < bitmap_len + sender_wire_len) {
    return std::nullopt;
  }
  auto endpoints = parse_bitmap(cur, bitmap_len);
  cur += bitmap_len;
  if (endpoints.size() != nep) {
    return std::nullopt;
  }
  const auto sender_bytes =
      maybe_decompress(cur, static_cast<size_t>(sender_wire_len), static_cast<size_t>(sender_len), sender_compressed);
  if (sender_bytes.size() != sender_len) {
    return std::nullopt;
  }
  cur += sender_wire_len;
  std::optional<ReliableHeaderLite> reliable;
  if (kTypeInfo[ty].reliable()) {
    if (static_cast<size_t>(end - cur) < kReliableHeaderBytes) {
      return std::nullopt;
    }
    ReliableHeaderLite hdr{};
    hdr.flags = cur[0];
    std::memcpy(&hdr.seq, cur + 1, 4);
    std::memcpy(&hdr.ack, cur + 5, 4);
    reliable = hdr;
  }
  return FrameInfoLite{
      TelemetryEnvelopeLite{static_cast<uint32_t>(ty), std::move(endpoints),
                            std::string(reinterpret_cast<const char *>(sender_bytes.data()), sender_bytes.size()), ts},
      reliable};
}

std::optional<size_t> reliable_header_offset(const uint8_t *bytes, size_t len) {
  if (bytes == nullptr || len < 2 + kCrcBytes) {
    return std::nullopt;
  }
  const uint8_t *cur = bytes;
  const uint8_t *end = bytes + len - kCrcBytes;
  const uint8_t flags = *cur++;
  const bool sender_compressed = (flags & kFlagCompressedSender) != 0u;
  cur++;
  uint64_t ty = 0, dsz = 0, ts = 0, sender_len = 0, sender_wire_len = 0;
  if (!read_uleb128(cur, end, ty) || !read_uleb128(cur, end, dsz) || !read_uleb128(cur, end, ts) ||
      !read_uleb128(cur, end, sender_len)) {
    return std::nullopt;
  }
  if (sender_compressed) {
    if (!read_uleb128(cur, end, sender_wire_len)) {
      return std::nullopt;
    }
  } else {
    sender_wire_len = sender_len;
  }
  const size_t bitmap_len = endpoint_bitmap_bytes();
  if (!valid_type(static_cast<uint32_t>(ty)) || static_cast<size_t>(end - cur) < bitmap_len + sender_wire_len) {
    return std::nullopt;
  }
  cur += bitmap_len + sender_wire_len;
  if (!kTypeInfo[ty].reliable()) {
    return std::nullopt;
  }
  return static_cast<size_t>(cur - bytes);
}

bool rewrite_reliable_header(uint8_t *bytes, size_t len, uint8_t flags, uint32_t seq, uint32_t ack) {
  const auto off = reliable_header_offset(bytes, len);
  if (!off || len < *off + kReliableHeaderBytes + kCrcBytes) {
    return false;
  }
  bytes[*off] = flags;
  std::memcpy(bytes + *off + 1, &seq, 4);
  std::memcpy(bytes + *off + 5, &ack, 4);
  const uint32_t crc = crc32_bytes(bytes, len - kCrcBytes);
  std::memcpy(bytes + len - kCrcBytes, &crc, 4);
  return true;
}

std::optional<uint64_t> packet_id_from_wire(const uint8_t *bytes, size_t len) {
  const auto pkt = deserialize_packet(bytes, len);
  if (!pkt) {
    return std::nullopt;
  }
  return packet_id(*pkt);
}

} // namespace seds
