#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace seds {

struct TopologySideRoute {
    size_t side_id{};
    std::string side_name;
    std::vector<uint32_t> reachable_endpoints;
    std::vector<std::string> reachable_timesync_sources;
    uint64_t last_seen_ms{};
    uint64_t age_ms{};
};

struct TopologySnapshot {
    std::vector<uint32_t> advertised_endpoints;
    std::vector<std::string> advertised_timesync_sources;
    std::vector<TopologySideRoute> routes;
    uint64_t current_announce_interval_ms{};
    uint64_t next_announce_ms{};
};

}  // namespace seds
