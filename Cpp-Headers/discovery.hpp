#pragma once

#include "discovery_helpers.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace seds {

struct TopologyAnnouncerRoute {
    std::string sender_id;
    std::vector<uint32_t> reachable_endpoints;
    std::vector<std::string> reachable_timesync_sources;
    std::vector<TopologyBoardNode> routers;
    uint64_t last_seen_ms{};
    uint64_t age_ms{};

    bool operator==(const TopologyAnnouncerRoute&) const = default;
};

struct TopologySideRoute {
    size_t side_id{};
    std::string side_name;
    std::vector<uint32_t> reachable_endpoints;
    std::vector<std::string> reachable_timesync_sources;
    std::vector<TopologyAnnouncerRoute> announcers;
    uint64_t last_seen_ms{};
    uint64_t age_ms{};

    bool operator==(const TopologySideRoute&) const = default;
};

struct TopologySnapshot {
    std::vector<uint32_t> advertised_endpoints;
    std::vector<std::string> advertised_timesync_sources;
    std::vector<TopologyBoardNode> routers;
    std::vector<TopologySideRoute> routes;
    uint64_t current_announce_interval_ms{};
    uint64_t next_announce_ms{};

    bool operator==(const TopologySnapshot&) const = default;
};

}  // namespace seds
