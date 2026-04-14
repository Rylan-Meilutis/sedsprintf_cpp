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

SedsResult Router::periodic(uint32_t timeout_ms) {
    return seds_router_periodic(router_.get(), timeout_ms);
}

SedsResult Router::periodic_no_timesync(uint32_t timeout_ms) {
    return seds_router_periodic_no_timesync(router_.get(), timeout_ms);
}

SedsResult Router::set_sender(std::string_view sender) {
    return seds_router_set_sender(router_.get(), sender.data(), sender.size());
}

SedsResult Router::get_network_time_ms(uint64_t& out_ms) const {
    return seds_router_get_network_time_ms(router_.get(), &out_ms);
}

SedsResult Router::get_network_time(SedsNetworkTime& out) const {
    return seds_router_get_network_time(router_.get(), &out);
}

SedsResult Router::configure_timesync(bool enabled) {
    return configure_timesync(enabled, TimeSyncOptions{});
}

SedsResult Router::configure_timesync(bool enabled, const TimeSyncOptions& options) {
    return seds_router_configure_timesync(router_.get(), enabled, options.role, options.priority,
                                          options.source_timeout_ms, options.announce_interval_ms,
                                          options.request_interval_ms);
}

SedsResult Router::poll_timesync(bool* out_did_queue) {
    return seds_router_poll_timesync(router_.get(), out_did_queue);
}

SedsResult Router::announce_discovery() {
    return seds_router_announce_discovery(router_.get());
}

SedsResult Router::poll_discovery(bool* out_did_queue) {
    return seds_router_poll_discovery(router_.get(), out_did_queue);
}

SedsResult Router::set_local_network_time(const SedsNetworkTime& time) {
    return seds_router_set_local_network_time(
        router_.get(), time.has_year, time.year, time.has_month, time.month, time.has_day, time.day,
        time.has_hour, time.hour, time.has_minute, time.minute, time.has_second, time.second,
        time.has_nanosecond, time.nanosecond);
}

SedsResult Router::set_local_network_date(int32_t year, uint8_t month, uint8_t day) {
    return seds_router_set_local_network_date(router_.get(), year, month, day);
}

SedsResult Router::set_local_network_time_hm(uint8_t hour, uint8_t minute) {
    return seds_router_set_local_network_time_hm(router_.get(), hour, minute);
}

SedsResult Router::set_local_network_time_hms(uint8_t hour, uint8_t minute, uint8_t second) {
    return seds_router_set_local_network_time_hms(router_.get(), hour, minute, second);
}

SedsResult Router::set_local_network_time_hms_millis(uint8_t hour, uint8_t minute, uint8_t second,
                                                     uint16_t millisecond) {
    return seds_router_set_local_network_time_hms_millis(router_.get(), hour, minute, second, millisecond);
}

SedsResult Router::set_local_network_time_hms_nanos(uint8_t hour, uint8_t minute, uint8_t second,
                                                    uint32_t nanosecond) {
    return seds_router_set_local_network_time_hms_nanos(router_.get(), hour, minute, second, nanosecond);
}

SedsResult Router::set_local_network_datetime(int32_t year, uint8_t month, uint8_t day, uint8_t hour,
                                              uint8_t minute, uint8_t second) {
    return seds_router_set_local_network_datetime(router_.get(), year, month, day, hour, minute, second);
}

SedsResult Router::set_local_network_datetime_millis(int32_t year, uint8_t month, uint8_t day, uint8_t hour,
                                                     uint8_t minute, uint8_t second, uint16_t millisecond) {
    return seds_router_set_local_network_datetime_millis(router_.get(), year, month, day, hour, minute, second,
                                                         millisecond);
}

SedsResult Router::set_local_network_datetime_nanos(int32_t year, uint8_t month, uint8_t day, uint8_t hour,
                                                    uint8_t minute, uint8_t second, uint32_t nanosecond) {
    return seds_router_set_local_network_datetime_nanos(router_.get(), year, month, day, hour, minute, second,
                                                        nanosecond);
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

TopologySnapshot export_topology_snapshot(const SedsRouter& raw_router_ref) {
    auto& raw_router = const_cast<SedsRouter&>(raw_router_ref);
    const uint64_t now_ms = raw_router.now_ms();
    std::scoped_lock lock(raw_router.mu);
    TopologySnapshot snapshot;
    for (const auto& [side_id, route] : raw_router.discovery_routes) {
        if (now_ms - route.last_seen_ms > kDiscoveryTtlMs) {
            continue;
        }
        TopologySideRoute item;
        item.side_id = static_cast<size_t>(side_id);
        if (side_id >= 0 && static_cast<size_t>(side_id) < raw_router.sides.size()) {
            item.side_name = raw_router.sides[side_id].name;
        }
        item.reachable_endpoints.assign(route.endpoints.begin(), route.endpoints.end());
        item.reachable_timesync_sources.assign(route.timesync_sources.begin(), route.timesync_sources.end());
        for (const auto& [sender_id, sender_state] : route.announcers) {
            TopologyAnnouncerRoute announcer;
            announcer.sender_id = sender_id;
            announcer.reachable_endpoints.assign(sender_state.endpoints.begin(), sender_state.endpoints.end());
            announcer.reachable_timesync_sources.assign(sender_state.timesync_sources.begin(),
                                                        sender_state.timesync_sources.end());
            announcer.routers = sender_state.topology_boards;
            announcer.last_seen_ms = sender_state.last_seen_ms;
            announcer.age_ms = now_ms - sender_state.last_seen_ms;
            item.announcers.push_back(std::move(announcer));
        }
        item.last_seen_ms = route.last_seen_ms;
        item.age_ms = now_ms - route.last_seen_ms;
        snapshot.routes.push_back(std::move(item));
    }
    snapshot.routers = advertised_discovery_topology_for_side(raw_router, -1);
    std::tie(snapshot.advertised_endpoints, snapshot.advertised_timesync_sources) =
        summarize_topology_boards(snapshot.routers);
    snapshot.current_announce_interval_ms = raw_router.discovery_interval_ms;
    snapshot.next_announce_ms = raw_router.discovery_next_ms;
    return snapshot;
}

TopologySnapshot Router::export_topology() const {
    return export_topology_snapshot(*router_);
}

}  // namespace seds
