#pragma once

#include "packet.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>

namespace seds {

struct TimeSyncAnnounceFields {
    uint64_t priority{};
    uint64_t time_ms{};
};

struct TimeSyncRequestFields {
    uint64_t seq{};
    uint64_t t1_ms{};
};

struct TimeSyncResponseFields {
    uint64_t seq{};
    uint64_t t1_ms{};
    uint64_t t2_ms{};
    uint64_t t3_ms{};
};

struct TimeSyncSample {
    int64_t offset_ms{};
    uint64_t delay_ms{};
};

inline Packet build_timesync_announce(std::string sender, uint64_t timestamp_ms, uint64_t priority, uint64_t time_ms) {
    const std::array<uint64_t, 2> payload{priority, time_ms};
    return Packet(SEDS_DT_TIME_SYNC_ANNOUNCE, {SEDS_EP_TIME_SYNC}, std::move(sender), timestamp_ms,
                  std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(payload.data()), sizeof(payload)));
}

inline Packet build_timesync_request(std::string sender, uint64_t timestamp_ms, uint64_t seq, uint64_t t1_ms) {
    const std::array<uint64_t, 2> payload{seq, t1_ms};
    return Packet(SEDS_DT_TIME_SYNC_REQUEST, {SEDS_EP_TIME_SYNC}, std::move(sender), timestamp_ms,
                  std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(payload.data()), sizeof(payload)));
}

inline Packet build_timesync_response(std::string sender, uint64_t timestamp_ms, uint64_t seq, uint64_t t1_ms,
                                      uint64_t t2_ms, uint64_t t3_ms) {
    const std::array<uint64_t, 4> payload{seq, t1_ms, t2_ms, t3_ms};
    return Packet(SEDS_DT_TIME_SYNC_RESPONSE, {SEDS_EP_TIME_SYNC}, std::move(sender), timestamp_ms,
                  std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(payload.data()), sizeof(payload)));
}

inline TimeSyncAnnounceFields decode_timesync_announce(const Packet& pkt) {
    if (pkt.type() != SEDS_DT_TIME_SYNC_ANNOUNCE || pkt.payload().size() != sizeof(uint64_t) * 2u) {
        throw std::invalid_argument("invalid timesync announce packet");
    }
    TimeSyncAnnounceFields fields{};
    std::memcpy(&fields, pkt.payload().data(), sizeof(fields));
    return fields;
}

inline TimeSyncRequestFields decode_timesync_request(const Packet& pkt) {
    if (pkt.type() != SEDS_DT_TIME_SYNC_REQUEST || pkt.payload().size() != sizeof(uint64_t) * 2u) {
        throw std::invalid_argument("invalid timesync request packet");
    }
    TimeSyncRequestFields fields{};
    std::memcpy(&fields, pkt.payload().data(), sizeof(fields));
    return fields;
}

inline TimeSyncResponseFields decode_timesync_response(const Packet& pkt) {
    if (pkt.type() != SEDS_DT_TIME_SYNC_RESPONSE || pkt.payload().size() != sizeof(uint64_t) * 4u) {
        throw std::invalid_argument("invalid timesync response packet");
    }
    TimeSyncResponseFields fields{};
    std::memcpy(&fields, pkt.payload().data(), sizeof(fields));
    return fields;
}

inline TimeSyncSample compute_offset_delay(uint64_t t1_ms, uint64_t t2_ms, uint64_t t3_ms, uint64_t t4_ms) {
    const auto t1 = static_cast<int64_t>(t1_ms);
    const auto t2 = static_cast<int64_t>(t2_ms);
    const auto t3 = static_cast<int64_t>(t3_ms);
    const auto t4 = static_cast<int64_t>(t4_ms);
    const int64_t offset = ((t2 - t1) + (t3 - t4)) / 2;
    const int64_t delay = (t4 - t1) - (t3 - t2);
    return TimeSyncSample{
        .offset_ms = offset,
        .delay_ms = delay < 0 ? 0u : static_cast<uint64_t>(delay),
    };
}

inline std::pair<uint64_t, uint64_t> estimate_network_time(uint64_t t1_ms, uint64_t t4_ms, uint64_t t2_network_ms,
                                                           uint64_t t3_network_ms) {
    const uint64_t round_trip = t4_ms >= t1_ms ? (t4_ms - t1_ms) : 0u;
    const uint64_t server_processing = t3_network_ms >= t2_network_ms ? (t3_network_ms - t2_network_ms) : 0u;
    const uint64_t one_way_delay = round_trip >= server_processing ? (round_trip - server_processing) / 2u : 0u;
    return {t3_network_ms + one_way_delay, one_way_delay};
}

}  // namespace seds
