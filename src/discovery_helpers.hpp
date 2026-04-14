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

struct TopologyBoardNode {
    std::string sender_id;
    std::vector<uint32_t> reachable_endpoints;
    std::vector<std::string> reachable_timesync_sources;
    std::vector<std::string> connections;

    bool operator==(const TopologyBoardNode&) const = default;
};

inline void sort_dedup_strings(std::vector<std::string>& items) {
    std::ranges::sort(items);
    items.erase(std::ranges::unique(items).begin(), items.end());
}

inline void normalize_topology_boards(std::vector<TopologyBoardNode>& boards) {
    for (auto& board : boards) {
        std::ranges::sort(board.reachable_endpoints);
        board.reachable_endpoints.erase(std::ranges::unique(board.reachable_endpoints).begin(),
                                        board.reachable_endpoints.end());
        sort_dedup_strings(board.reachable_timesync_sources);
        board.connections.erase(std::ranges::remove(board.connections, board.sender_id).begin(), board.connections.end());
        sort_dedup_strings(board.connections);
    }
    std::ranges::sort(boards, {}, &TopologyBoardNode::sender_id);
    boards.erase(std::ranges::unique(boards, {}, &TopologyBoardNode::sender_id).begin(), boards.end());
}

inline void merge_topology_boards(std::vector<TopologyBoardNode>& dst, std::span<const TopologyBoardNode> src) {
    for (const auto& board : src) {
        auto it = std::ranges::find(dst, board.sender_id, &TopologyBoardNode::sender_id);
        if (it == dst.end()) {
            dst.push_back(board);
            continue;
        }
        it->reachable_endpoints.insert(it->reachable_endpoints.end(),
                                       board.reachable_endpoints.begin(),
                                       board.reachable_endpoints.end());
        it->reachable_timesync_sources.insert(it->reachable_timesync_sources.end(),
                                             board.reachable_timesync_sources.begin(),
                                             board.reachable_timesync_sources.end());
        it->connections.insert(it->connections.end(), board.connections.begin(), board.connections.end());
    }
    normalize_topology_boards(dst);
}

inline std::pair<std::vector<uint32_t>, std::vector<std::string>>
summarize_topology_boards(std::span<const TopologyBoardNode> boards) {
    std::vector<uint32_t> reachable_endpoints;
    std::vector<std::string> reachable_timesync_sources;
    for (const auto& board : boards) {
        reachable_endpoints.insert(reachable_endpoints.end(),
                                   board.reachable_endpoints.begin(),
                                   board.reachable_endpoints.end());
        reachable_timesync_sources.insert(reachable_timesync_sources.end(),
                                          board.reachable_timesync_sources.begin(),
                                          board.reachable_timesync_sources.end());
    }
    std::ranges::sort(reachable_endpoints);
    reachable_endpoints.erase(std::ranges::unique(reachable_endpoints).begin(), reachable_endpoints.end());
    sort_dedup_strings(reachable_timesync_sources);
    return {std::move(reachable_endpoints), std::move(reachable_timesync_sources)};
}

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

inline Packet build_discovery_topology(std::string sender, uint64_t timestamp_ms,
                                       std::span<const TopologyBoardNode> boards) {
    std::vector<uint8_t> payload;
    std::vector<TopologyBoardNode> normalized(boards.begin(), boards.end());
    normalize_topology_boards(normalized);

    const auto count = static_cast<uint32_t>(normalized.size());
    payload.resize(sizeof(count));
    std::memcpy(payload.data(), &count, sizeof(count));
    for (const auto& board : normalized) {
        const auto append_string = [&payload](std::string_view value) {
            const auto len = static_cast<uint32_t>(value.size());
            const size_t old_size = payload.size();
            payload.resize(old_size + sizeof(len) + value.size());
            std::memcpy(payload.data() + old_size, &len, sizeof(len));
            std::memcpy(payload.data() + old_size + sizeof(len), value.data(), value.size());
        };

        append_string(board.sender_id);

        const auto endpoint_count = static_cast<uint32_t>(board.reachable_endpoints.size());
        const size_t endpoint_off = payload.size();
        payload.resize(endpoint_off + sizeof(endpoint_count) + board.reachable_endpoints.size() * sizeof(uint32_t));
        std::memcpy(payload.data() + endpoint_off, &endpoint_count, sizeof(endpoint_count));
        std::memcpy(payload.data() + endpoint_off + sizeof(endpoint_count),
                    board.reachable_endpoints.data(),
                    board.reachable_endpoints.size() * sizeof(uint32_t));

        const auto source_count = static_cast<uint32_t>(board.reachable_timesync_sources.size());
        const size_t source_off = payload.size();
        payload.resize(source_off + sizeof(source_count));
        std::memcpy(payload.data() + source_off, &source_count, sizeof(source_count));
        for (const auto& source : board.reachable_timesync_sources) {
            append_string(source);
        }

        const auto connection_count = static_cast<uint32_t>(board.connections.size());
        const size_t connection_off = payload.size();
        payload.resize(connection_off + sizeof(connection_count));
        std::memcpy(payload.data() + connection_off, &connection_count, sizeof(connection_count));
        for (const auto& peer : board.connections) {
            append_string(peer);
        }
    }

    return Packet(SEDS_DT_DISCOVERY_TOPOLOGY, {SEDS_EP_DISCOVERY}, std::move(sender), timestamp_ms, payload);
}

inline std::string decode_sized_string(std::span<const uint8_t> payload, size_t& offset, const char* error) {
    if (offset + sizeof(uint32_t) > payload.size()) {
        throw std::invalid_argument(error);
    }
    uint32_t len = 0;
    std::memcpy(&len, payload.data() + offset, sizeof(len));
    offset += sizeof(len);
    if (offset + len > payload.size()) {
        throw std::invalid_argument(error);
    }
    std::string out(reinterpret_cast<const char*>(payload.data() + offset), len);
    offset += len;
    return out;
}

inline std::vector<TopologyBoardNode> decode_discovery_topology_payload(std::span<const uint8_t> payload) {
    if (payload.size() < sizeof(uint32_t)) {
        throw std::invalid_argument("invalid discovery topology payload");
    }
    uint32_t count = 0;
    std::memcpy(&count, payload.data(), sizeof(count));
    size_t offset = sizeof(count);
    std::vector<TopologyBoardNode> boards;
    boards.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        TopologyBoardNode board;
        board.sender_id = decode_sized_string(payload, offset, "invalid discovery topology sender");

        if (offset + sizeof(uint32_t) > payload.size()) {
            throw std::invalid_argument("invalid discovery topology endpoint count");
        }
        uint32_t endpoint_count = 0;
        std::memcpy(&endpoint_count, payload.data() + offset, sizeof(endpoint_count));
        offset += sizeof(endpoint_count);
        if (offset + endpoint_count * sizeof(uint32_t) > payload.size()) {
            throw std::invalid_argument("invalid discovery topology endpoints");
        }
        board.reachable_endpoints.resize(endpoint_count);
        std::memcpy(board.reachable_endpoints.data(), payload.data() + offset, endpoint_count * sizeof(uint32_t));
        offset += endpoint_count * sizeof(uint32_t);
        board.reachable_endpoints.erase(std::ranges::remove(board.reachable_endpoints, SEDS_EP_DISCOVERY).begin(),
                                        board.reachable_endpoints.end());

        if (offset + sizeof(uint32_t) > payload.size()) {
            throw std::invalid_argument("invalid discovery topology source count");
        }
        uint32_t source_count = 0;
        std::memcpy(&source_count, payload.data() + offset, sizeof(source_count));
        offset += sizeof(source_count);
        board.reachable_timesync_sources.reserve(source_count);
        for (uint32_t source_i = 0; source_i < source_count; ++source_i) {
            auto source = decode_sized_string(payload, offset, "invalid discovery topology source");
            if (!source.empty()) {
                board.reachable_timesync_sources.push_back(std::move(source));
            }
        }

        if (offset + sizeof(uint32_t) > payload.size()) {
            throw std::invalid_argument("invalid discovery topology connection count");
        }
        uint32_t connection_count = 0;
        std::memcpy(&connection_count, payload.data() + offset, sizeof(connection_count));
        offset += sizeof(connection_count);
        board.connections.reserve(connection_count);
        for (uint32_t connection_i = 0; connection_i < connection_count; ++connection_i) {
            auto peer = decode_sized_string(payload, offset, "invalid discovery topology connection");
            if (!peer.empty()) {
                board.connections.push_back(std::move(peer));
            }
        }
        boards.push_back(std::move(board));
    }
    if (offset != payload.size()) {
        throw std::invalid_argument("invalid discovery topology trailing bytes");
    }
    normalize_topology_boards(boards);
    return boards;
}

inline std::vector<TopologyBoardNode> decode_discovery_topology(const Packet& pkt) {
    if (pkt.type() != SEDS_DT_DISCOVERY_TOPOLOGY) {
        throw std::invalid_argument("invalid discovery topology packet");
    }
    return decode_discovery_topology_payload(pkt.payload());
}

}  // namespace seds
