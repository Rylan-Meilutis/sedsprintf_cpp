#include "relay.hpp"

#include "internal.hpp"

#include <algorithm>
#include <ranges>
#include <stdexcept>

namespace seds {

struct Relay::State {
    std::vector<std::unique_ptr<PacketHandler>> packet_handlers;
    std::vector<std::unique_ptr<SerializedHandler>> serialized_handlers;
};

Relay::Relay() : state_(std::make_shared<State>()), relay_(seds_relay_new(nullptr, nullptr)) {
    if (!relay_) {
        throw std::runtime_error("failed to create relay");
    }
}

SedsResult Relay::packet_bridge(const SedsPacketView* pkt, void* user) {
    const auto* fn = static_cast<PacketHandler*>(user);
    return (*fn)(Packet::from_view(*pkt));
}

SedsResult Relay::serialized_bridge(const uint8_t* bytes, size_t len, void* user) {
    const auto* fn = static_cast<SerializedHandler*>(user);
    return (*fn)(std::span<const uint8_t>(bytes, len));
}

int32_t Relay::add_side_serialized(std::string name, SerializedHandler handler, bool reliable) {
    return add_side_serialized(name, std::move(handler), SideOptions{reliable, false});
}

int32_t Relay::add_side_serialized(const std::string& name, SerializedHandler handler, SideOptions options) {
    state_->serialized_handlers.push_back(std::make_unique<SerializedHandler>(std::move(handler)));
    const auto side_id = seds_relay_add_side_serialized(relay_.get(), name.c_str(), name.size(), serialized_bridge,
                                                        state_->serialized_handlers.back().get(),
                                                        options.reliable_enabled);
    if (side_id >= 0 && options.link_local_enabled) {
        seds_relay_set_side_link_local_enabled(relay_.get(), side_id, true);
    }
    return side_id;
}

int32_t Relay::add_side_packet(std::string name, PacketHandler handler, bool reliable) {
    return add_side_packet(name, std::move(handler), SideOptions{reliable, false});
}

int32_t Relay::add_side_packet(const std::string& name, PacketHandler handler, SideOptions options) {
    state_->packet_handlers.push_back(std::make_unique<PacketHandler>(std::move(handler)));
    const auto side_id = seds_relay_add_side_packet(relay_.get(), name.c_str(), name.size(), packet_bridge,
                                                    state_->packet_handlers.back().get(),
                                                    options.reliable_enabled);
    if (side_id >= 0 && options.link_local_enabled) {
        seds_relay_set_side_link_local_enabled(relay_.get(), side_id, true);
    }
    return side_id;
}

SedsResult Relay::receive_from_side(uint32_t side_id, const Packet& packet) {
    const auto view = packet.view();
    return seds_relay_rx_packet_from_side(relay_.get(), side_id, &view);
}

SedsResult Relay::process_all() {
    return seds_relay_process_all_queues(relay_.get());
}

SedsResult Relay::announce_discovery() {
    return seds_relay_announce_discovery(relay_.get());
}

SedsResult Relay::poll_discovery(bool* out_did_queue) {
    return seds_relay_poll_discovery(relay_.get(), out_did_queue);
}

SedsResult Relay::periodic(uint32_t timeout_ms) {
    return seds_relay_periodic(relay_.get(), timeout_ms);
}

TopologySnapshot Relay::export_topology() const {
    const auto* raw_relay = static_cast<const SedsRelay*>(relay_.get());
    const uint64_t now_ms = raw_relay->now_ms();
    std::scoped_lock lock(raw_relay->mu);
    TopologySnapshot snapshot;
    for (const auto& [side_id, route] : raw_relay->discovery_routes) {
        if (now_ms - route.last_seen_ms > kDiscoveryTtlMs) {
            continue;
        }
        TopologySideRoute item;
        item.side_id = static_cast<size_t>(side_id);
        if (side_id >= 0 && static_cast<size_t>(side_id) < raw_relay->sides.size()) {
            item.side_name = raw_relay->sides[side_id].name;
        }
        item.reachable_endpoints.assign(route.endpoints.begin(), route.endpoints.end());
        item.reachable_timesync_sources.assign(route.timesync_sources.begin(), route.timesync_sources.end());
        item.last_seen_ms = route.last_seen_ms;
        item.age_ms = now_ms - route.last_seen_ms;
        snapshot.routes.push_back(std::move(item));
    }
    for (const auto& [_, route] : raw_relay->discovery_routes) {
        if (now_ms - route.last_seen_ms > kDiscoveryTtlMs) {
            continue;
        }
        snapshot.advertised_endpoints.insert(snapshot.advertised_endpoints.end(), route.endpoints.begin(), route.endpoints.end());
        snapshot.advertised_timesync_sources.insert(snapshot.advertised_timesync_sources.end(), route.timesync_sources.begin(), route.timesync_sources.end());
    }
    std::ranges::sort(snapshot.advertised_endpoints);
    snapshot.advertised_endpoints.erase(std::ranges::unique(snapshot.advertised_endpoints).begin(),
                                        snapshot.advertised_endpoints.end());
    std::ranges::sort(snapshot.advertised_timesync_sources);
    snapshot.advertised_timesync_sources.erase(
        std::ranges::unique(snapshot.advertised_timesync_sources).begin(),
        snapshot.advertised_timesync_sources.end());
    snapshot.current_announce_interval_ms = raw_relay->discovery_interval_ms;
    snapshot.next_announce_ms = raw_relay->discovery_next_ms;
    return snapshot;
}

}  // namespace seds
