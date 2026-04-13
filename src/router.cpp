#include "router.hpp"

#include "internal.hpp"

#include <algorithm>
#include <ranges>
#include <stdexcept>

namespace seds {

struct Router::State {
    std::vector<std::unique_ptr<PacketHandler>> packet_handlers;
    std::vector<std::unique_ptr<SerializedHandler>> serialized_handlers;
};

Router::Router(SedsRouterMode mode) : state_(std::make_shared<State>()), router_(seds_router_new(mode, nullptr, nullptr, nullptr, 0)) {
    if (!router_) {
        throw std::runtime_error("failed to create router");
    }
}

SedsResult Router::packet_bridge(const SedsPacketView* pkt, void* user) {
    const auto* fn = static_cast<PacketHandler*>(user);
    return (*fn)(Packet::from_view(*pkt));
}

SedsResult Router::serialized_bridge(const uint8_t* bytes, size_t len, void* user) {
    const auto* fn = static_cast<SerializedHandler*>(user);
    return (*fn)(std::span<const uint8_t>(bytes, len));
}

int32_t Router::add_side_serialized(std::string name, SerializedHandler handler, bool reliable) {
    return add_side_serialized(name, std::move(handler), SideOptions{reliable, false});
}

int32_t Router::add_side_serialized(const std::string& name, SerializedHandler handler, SideOptions options) {
    state_->serialized_handlers.push_back(std::make_unique<SerializedHandler>(std::move(handler)));
    const auto side_id = seds_router_add_side_serialized(router_.get(), name.c_str(), name.size(), serialized_bridge,
                                                         state_->serialized_handlers.back().get(),
                                                         options.reliable_enabled);
    if (side_id >= 0 && options.link_local_enabled) {
        seds_router_set_side_link_local_enabled(router_.get(), side_id, true);
    }
    return side_id;
}

int32_t Router::add_side_packet(std::string name, PacketHandler handler, bool reliable) {
    return add_side_packet(name, std::move(handler), SideOptions{reliable, false});
}

int32_t Router::add_side_packet(const std::string& name, PacketHandler handler, SideOptions options) {
    state_->packet_handlers.push_back(std::make_unique<PacketHandler>(std::move(handler)));
    const auto side_id = seds_router_add_side_packet(router_.get(), name.c_str(), name.size(), packet_bridge,
                                                     state_->packet_handlers.back().get(),
                                                     options.reliable_enabled);
    if (side_id >= 0 && options.link_local_enabled) {
        seds_router_set_side_link_local_enabled(router_.get(), side_id, true);
    }
    return side_id;
}

SedsResult Router::log(const Packet& packet, bool queue) {
    const auto view = packet.view();
    if (queue) {
        return seds_router_transmit_message_queue(router_.get(), &view);
    }
    return seds_router_transmit_message(router_.get(), &view);
}

SedsResult Router::receive(const Packet& packet) {
    const auto view = packet.view();
    return seds_router_receive(router_.get(), &view);
}

SedsResult Router::process_all() {
    return seds_router_process_all_queues(router_.get());
}

SedsResult Router::set_sender(std::string_view sender) {
    return seds_router_set_sender(router_.get(), sender.data(), sender.size());
}

SedsResult Router::clear_local_network_time() {
    auto* raw_router = static_cast<SedsRouter*>(router_.get());
    std::scoped_lock lock(raw_router->mu);
    raw_router->timesync.has_network_time = false;
    raw_router->timesync.current_source.clear();
    raw_router->timesync.current_source_priority = UINT64_MAX;
    return SEDS_OK;
}

SedsResult Router::clear_network_time_source(std::string_view source) {
    auto* raw_router = static_cast<SedsRouter*>(router_.get());
    std::scoped_lock lock(raw_router->mu);
    raw_router->timesync.sources.erase(std::string(source));
    if (raw_router->timesync.current_source == source) {
        raw_router->timesync.current_source.clear();
        raw_router->timesync.current_source_priority = UINT64_MAX;
    }
    return SEDS_OK;
}

TopologySnapshot Router::export_topology() const {
    const auto* raw_router = static_cast<const SedsRouter*>(router_.get());
    const uint64_t now_ms = raw_router->now_ms();
    std::scoped_lock lock(raw_router->mu);
    TopologySnapshot snapshot;
    for (uint32_t ep = 0; ep < kEndpointCount; ++ep) {
        bool advertise = false;
        for (const auto& local : raw_router->locals) {
            if (local.endpoint == ep) {
                advertise = true;
                break;
            }
        }
        if (advertise) {
            snapshot.advertised_endpoints.push_back(ep);
        }
    }
    if (raw_router->timesync.enabled) {
        snapshot.advertised_endpoints.push_back(SEDS_EP_TIME_SYNC);
    }
    if (raw_router->timesync.has_network_time) {
        snapshot.advertised_timesync_sources.push_back(raw_router->timesync.current_source.empty() ? "local" : raw_router->timesync.current_source);
    }
    for (const auto& [side_id, route] : raw_router->discovery_routes) {
        if (now_ms - route.last_seen_ms > kDiscoveryTtlMs) {
            continue;
        }
        TopologySideRoute item;
        item.side_id = static_cast<size_t>(side_id);
        if (side_id >= 0 && static_cast<size_t>(side_id) < raw_router->sides.size()) {
            item.side_name = raw_router->sides[side_id].name;
        }
        item.reachable_endpoints.assign(route.endpoints.begin(), route.endpoints.end());
        item.reachable_timesync_sources.assign(route.timesync_sources.begin(), route.timesync_sources.end());
        item.last_seen_ms = route.last_seen_ms;
        item.age_ms = now_ms - route.last_seen_ms;
        snapshot.routes.push_back(std::move(item));
        snapshot.advertised_endpoints.insert(snapshot.advertised_endpoints.end(), route.endpoints.begin(), route.endpoints.end());
        snapshot.advertised_timesync_sources.insert(snapshot.advertised_timesync_sources.end(),
                                                    route.timesync_sources.begin(), route.timesync_sources.end());
    }
    std::ranges::sort(snapshot.advertised_endpoints);
    snapshot.advertised_endpoints.erase(
        std::ranges::unique(snapshot.advertised_endpoints).begin(),
        snapshot.advertised_endpoints.end());
    std::ranges::sort(snapshot.advertised_timesync_sources);
    snapshot.advertised_timesync_sources.erase(
        std::ranges::unique(snapshot.advertised_timesync_sources).begin(),
        snapshot.advertised_timesync_sources.end());
    snapshot.current_announce_interval_ms = raw_router->discovery_interval_ms;
    snapshot.next_announce_ms = raw_router->discovery_next_ms;
    return snapshot;
}

}  // namespace seds
