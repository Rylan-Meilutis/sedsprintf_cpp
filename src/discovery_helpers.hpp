#pragma once

#include "packet.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace seds {

inline Packet build_discovery_announce(std::string sender, uint64_t timestamp_ms, std::span<const uint32_t> endpoints) {
    std::vector<uint8_t> payload(endpoints.size() * sizeof(uint32_t));
    std::memcpy(payload.data(), endpoints.data(), payload.size());
    return Packet(SEDS_DT_DISCOVERY_ANNOUNCE, {SEDS_EP_DISCOVERY}, std::move(sender), timestamp_ms, payload);
}

inline std::vector<uint32_t> decode_discovery_payload(std::span<const uint8_t> payload) {
    if ((payload.size() % sizeof(uint32_t)) != 0u) {
        throw std::invalid_argument("invalid discovery payload width");
    }
    std::vector<uint32_t> endpoints(payload.size() / sizeof(uint32_t));
    std::memcpy(endpoints.data(), payload.data(), payload.size());
    endpoints.erase(std::ranges::remove(endpoints, SEDS_EP_DISCOVERY).begin(), endpoints.end());
    std::ranges::sort(endpoints);
    endpoints.erase(std::ranges::unique(endpoints).begin(), endpoints.end());
    return endpoints;
}

inline std::vector<uint32_t> decode_discovery_announce(const Packet& pkt) {
    if (pkt.type() != SEDS_DT_DISCOVERY_ANNOUNCE) {
        throw std::invalid_argument("invalid discovery announce packet");
    }
    return decode_discovery_payload(pkt.payload());
}

inline Packet build_discovery_timesync_sources(std::string sender, uint64_t timestamp_ms,
                                               std::span<const std::string_view> sources) {
    std::vector<std::string> deduped;
    deduped.reserve(sources.size());
    for (const auto source : sources) {
        deduped.emplace_back(source);
    }
    std::ranges::sort(deduped);
    deduped.erase(std::ranges::unique(deduped).begin(), deduped.end());

    std::vector<uint8_t> payload;
    const auto count = static_cast<uint32_t>(deduped.size());
    payload.resize(sizeof(count));
    std::memcpy(payload.data(), &count, sizeof(count));
    for (const auto& source : deduped) {
        const auto len = static_cast<uint32_t>(source.size());
        const size_t old_size = payload.size();
        payload.resize(old_size + sizeof(len) + len);
        std::memcpy(payload.data() + old_size, &len, sizeof(len));
        std::memcpy(payload.data() + old_size + sizeof(len), source.data(), len);
    }
    return Packet(SEDS_DT_DISCOVERY_TIMESYNC_SOURCES, {SEDS_EP_DISCOVERY}, std::move(sender), timestamp_ms, payload);
}

inline std::vector<std::string> decode_discovery_timesync_sources_payload(std::span<const uint8_t> payload) {
    if (payload.size() < sizeof(uint32_t)) {
        throw std::invalid_argument("invalid timesync sources payload");
    }
    uint32_t count = 0;
    std::memcpy(&count, payload.data(), sizeof(count));
    size_t offset = sizeof(count);
    std::vector<std::string> sources;
    sources.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        if (offset + sizeof(uint32_t) > payload.size()) {
            throw std::invalid_argument("truncated timesync sources payload");
        }
        uint32_t len = 0;
        std::memcpy(&len, payload.data() + offset, sizeof(len));
        offset += sizeof(len);
        if (offset + len > payload.size()) {
            throw std::invalid_argument("truncated timesync source string");
        }
        sources.emplace_back(reinterpret_cast<const char*>(payload.data() + offset), len);
        offset += len;
    }
    std::ranges::sort(sources);
    sources.erase(std::ranges::unique(sources).begin(), sources.end());
    return sources;
}

inline std::vector<std::string> decode_discovery_timesync_sources(const Packet& pkt) {
    if (pkt.type() != SEDS_DT_DISCOVERY_TIMESYNC_SOURCES) {
        throw std::invalid_argument("invalid discovery timesync sources packet");
    }
    return decode_discovery_timesync_sources_payload(pkt.payload());
}

}  // namespace seds
