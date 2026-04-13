#include "internal.hpp"

#include <algorithm>
#include <cstring>
#include <ranges>

namespace seds {
namespace {

template <typename RouteMapT>
bool prune_discovery_routes(RouteMapT& routes, uint64_t now_ms) {
    bool changed = false;
    for (auto it = routes.begin(); it != routes.end();) {
        auto &route = it->second;
        for (auto sender_it = route.announcers.begin(); sender_it != route.announcers.end();) {
            if (now_ms - sender_it->second.last_seen_ms > kDiscoveryTtlMs) {
                sender_it = route.announcers.erase(sender_it);
                changed = true;
            } else {
                ++sender_it;
            }
        }
        route.endpoints.clear();
        route.timesync_sources.clear();
        route.last_seen_ms = 0;
        for (const auto &[_, sender] : route.announcers) {
            route.endpoints.insert(sender.endpoints.begin(), sender.endpoints.end());
            route.timesync_sources.insert(sender.timesync_sources.begin(), sender.timesync_sources.end());
            route.last_seen_ms = std::max(route.last_seen_ms, sender.last_seen_ms);
        }
        if (route.announcers.empty() || now_ms - route.last_seen_ms > kDiscoveryTtlMs) {
            it = routes.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    return changed;
}

template <typename OwnerT>
void note_discovery_topology_change(OwnerT& owner, uint64_t now_ms) {
    owner.discovery_interval_ms = kDiscoveryFastMs;
    owner.discovery_next_ms = now_ms;
}

std::vector<uint32_t> decode_discovery_endpoints(const PacketData& pkt) {
    std::vector<uint32_t> out;
    for (size_t i = 0; i + 4 <= pkt.payload.size(); i += 4) {
        uint32_t ep = 0;
        std::memcpy(&ep, pkt.payload.data() + i, 4);
        if (valid_endpoint(ep) && ep != SEDS_EP_DISCOVERY) {
            out.push_back(ep);
        }
    }
    std::ranges::sort(out);
    out.erase(std::ranges::unique(out).begin(), out.end());
    return out;
}

std::vector<std::string> decode_discovery_timesync_sources(const PacketData& pkt) {
    std::vector<std::string> out;
    if (pkt.payload.size() < 4) {
        return out;
    }
    uint32_t count = 0;
    std::memcpy(&count, pkt.payload.data(), 4);
    size_t off = 4;
    for (uint32_t i = 0; i < count && off + 4 <= pkt.payload.size(); ++i) {
        uint32_t len = 0;
        std::memcpy(&len, pkt.payload.data() + off, 4);
        off += 4;
        if (off + len > pkt.payload.size()) {
            break;
        }
        if (len != 0) {
            out.emplace_back(reinterpret_cast<const char*>(pkt.payload.data() + off), len);
        }
        off += len;
    }
    std::ranges::sort(out);
    out.erase(std::ranges::unique(out).begin(), out.end());
    return out;
}

std::vector<uint32_t> router_local_discovery_endpoints(const SedsRouter& r, bool link_local_enabled) {
    std::vector<uint32_t> out;
    for (const auto& local : r.locals) {
        if (local.endpoint != SEDS_EP_DISCOVERY &&
            (link_local_enabled || !endpoint_link_local_only(local.endpoint))) {
            out.push_back(local.endpoint);
        }
    }
    if (r.timesync.enabled) {
        out.push_back(SEDS_EP_TIME_SYNC);
    }
    std::ranges::sort(out);
    out.erase(std::ranges::unique(out).begin(), out.end());
    return out;
}

std::vector<uint32_t> advertised_router_endpoints_for_side(const SedsRouter& r, int32_t dst_side) {
    const bool link_local_enabled =
        dst_side >= 0 && static_cast<size_t>(dst_side) < r.sides.size() && r.sides[dst_side].link_local_enabled;
    std::vector<uint32_t> out = router_local_discovery_endpoints(r, link_local_enabled);
    for (const auto& [side_id, route] : r.discovery_routes) {
        for (uint32_t ep : route.endpoints) {
            if (link_local_enabled || !endpoint_link_local_only(ep)) {
                out.push_back(ep);
            }
        }
    }
    std::ranges::sort(out);
    out.erase(std::ranges::unique(out).begin(), out.end());
    return out;
}

std::vector<std::string> advertised_router_timesync_sources_for_side(const SedsRouter& r, int32_t /*dst_side*/) {
    std::vector<std::string> out;
    if (r.timesync.enabled && r.timesync.has_network_time) {
        out.push_back(r.timesync.current_source.empty() ? "local" : r.timesync.current_source);
    }
    for (const auto& [side_id, route] : r.discovery_routes) {
        static_cast<void>(side_id);
        out.insert(out.end(), route.timesync_sources.begin(), route.timesync_sources.end());
    }
    std::ranges::sort(out);
    out.erase(std::ranges::unique(out).begin(), out.end());
    return out;
}

std::vector<uint32_t> advertised_relay_endpoints_for_side(const SedsRelay& r, int32_t dst_side) {
    const bool link_local_enabled =
        dst_side >= 0 && static_cast<size_t>(dst_side) < r.sides.size() && r.sides[dst_side].link_local_enabled;
    std::vector<uint32_t> out;
    for (const auto& [side_id, route] : r.discovery_routes) {
        static_cast<void>(side_id);
        for (uint32_t ep : route.endpoints) {
            if (link_local_enabled || !endpoint_link_local_only(ep)) {
                out.push_back(ep);
            }
        }
    }
    std::ranges::sort(out);
    out.erase(std::ranges::unique(out).begin(), out.end());
    return out;
}

std::vector<std::string> advertised_relay_timesync_sources_for_side(const SedsRelay& r, int32_t /*dst_side*/) {
    std::vector<std::string> out;
    for (const auto& [side_id, route] : r.discovery_routes) {
        static_cast<void>(side_id);
        out.insert(out.end(), route.timesync_sources.begin(), route.timesync_sources.end());
    }
    std::ranges::sort(out);
    out.erase(std::ranges::unique(out).begin(), out.end());
    return out;
}

}  // namespace

void queue_discovery_packets(SedsRouter& r) {
    const uint64_t now = r.now_ms();
    if (prune_discovery_routes(r.discovery_routes, now)) {
        note_discovery_topology_change(r, now);
    }
    for (size_t side_id = 0; side_id < r.sides.size(); ++side_id) {
        if (!r.sides[side_id].egress_enabled) {
            continue;
        }
        auto timesync_sources = advertised_router_timesync_sources_for_side(r, static_cast<int32_t>(side_id));
        if (!timesync_sources.empty()) {
            std::vector<uint8_t> ts_payload;
            append_le<uint32_t>(static_cast<uint32_t>(timesync_sources.size()), ts_payload);
            for (const auto& source : timesync_sources) {
                append_le<uint32_t>(static_cast<uint32_t>(source.size()), ts_payload);
                ts_payload.insert(ts_payload.end(), source.begin(), source.end());
            }
            auto pkt = make_internal_packet(SEDS_DT_DISCOVERY_TIMESYNC_SOURCES, now, std::move(ts_payload));
            pkt.sender = r.sender;
            enqueue_tx_front(r.tx_queue, r.tx_queue_bytes,
                             {std::move(pkt), std::nullopt, static_cast<int32_t>(side_id), false});
        }
        std::vector<uint8_t> payload;
        for (const uint32_t ep : advertised_router_endpoints_for_side(r, static_cast<int32_t>(side_id))) {
            append_le<uint32_t>(ep, payload);
        }
        auto pkt = make_internal_packet(SEDS_DT_DISCOVERY_ANNOUNCE, now, std::move(payload));
        pkt.sender = r.sender;
        enqueue_tx_front(r.tx_queue, r.tx_queue_bytes,
                         {std::move(pkt), std::nullopt, static_cast<int32_t>(side_id), false});
    }
    r.discovery_next_ms = now + r.discovery_interval_ms;
    r.discovery_interval_ms = std::min<uint64_t>(r.discovery_interval_ms * 2u, kDiscoverySlowMs);
}

void queue_discovery_packets(SedsRelay& r) {
    const uint64_t now = r.now_ms();
    if (prune_discovery_routes(r.discovery_routes, now)) {
        note_discovery_topology_change(r, now);
    }
    for (size_t side_id = 0; side_id < r.sides.size(); ++side_id) {
        if (!r.sides[side_id].egress_enabled) {
            continue;
        }
        auto timesync_sources = advertised_relay_timesync_sources_for_side(r, static_cast<int32_t>(side_id));
        if (!timesync_sources.empty()) {
            std::vector<uint8_t> ts_payload;
            append_le<uint32_t>(static_cast<uint32_t>(timesync_sources.size()), ts_payload);
            for (const auto& source : timesync_sources) {
                append_le<uint32_t>(static_cast<uint32_t>(source.size()), ts_payload);
                ts_payload.insert(ts_payload.end(), source.begin(), source.end());
            }
            auto pkt = make_internal_packet(SEDS_DT_DISCOVERY_TIMESYNC_SOURCES, now, std::move(ts_payload));
            pkt.sender = "RELAY";
            enqueue_tx_front(r.tx_queue, r.tx_queue_bytes,
                             {std::move(pkt), std::nullopt, static_cast<int32_t>(side_id), false});
        }
        std::vector<uint8_t> payload;
        for (const uint32_t ep : advertised_relay_endpoints_for_side(r, static_cast<int32_t>(side_id))) {
            append_le<uint32_t>(ep, payload);
        }
        auto pkt = make_internal_packet(SEDS_DT_DISCOVERY_ANNOUNCE, now, std::move(payload));
        pkt.sender = "RELAY";
        enqueue_tx_front(r.tx_queue, r.tx_queue_bytes,
                         {std::move(pkt), std::nullopt, static_cast<int32_t>(side_id), false});
    }
    r.discovery_next_ms = now + r.discovery_interval_ms;
    r.discovery_interval_ms = std::min<uint64_t>(r.discovery_interval_ms * 2u, kDiscoverySlowMs);
}

void handle_discovery_packet(SedsRouter& r, const PacketData& pkt, std::optional<int32_t> src_side) {
    if (!src_side || pkt.sender == r.sender) {
        return;
    }
    DiscoveryRoute next = r.discovery_routes[*src_side];
    auto &sender_state = next.announcers[pkt.sender];
    sender_state.last_seen_ms = r.now_ms();
    bool changed = false;
    if (pkt.ty == SEDS_DT_DISCOVERY_ANNOUNCE) {
        std::unordered_set<uint32_t> eps;
        for (uint32_t ep : decode_discovery_endpoints(pkt)) {
            eps.insert(ep);
        }
        changed = sender_state.endpoints != eps;
        sender_state.endpoints = std::move(eps);
    } else if (pkt.ty == SEDS_DT_DISCOVERY_TIMESYNC_SOURCES) {
        std::unordered_set<std::string> sources;
        for (auto& source : decode_discovery_timesync_sources(pkt)) {
            sources.insert(std::move(source));
        }
        changed = sender_state.timesync_sources != sources;
        sender_state.timesync_sources = std::move(sources);
    }
    next.endpoints.clear();
    next.timesync_sources.clear();
    next.last_seen_ms = 0;
    for (const auto &[_, sender] : next.announcers) {
        next.endpoints.insert(sender.endpoints.begin(), sender.endpoints.end());
        next.timesync_sources.insert(sender.timesync_sources.begin(), sender.timesync_sources.end());
        next.last_seen_ms = std::max(next.last_seen_ms, sender.last_seen_ms);
    }
    r.discovery_routes[*src_side] = std::move(next);
    if (changed) {
        note_discovery_topology_change(r, r.now_ms());
    }
}

void handle_discovery_packet(SedsRelay& r, const PacketData& pkt, std::optional<int32_t> src_side) {
    if (!src_side) {
        return;
    }
    DiscoveryRoute next = r.discovery_routes[*src_side];
    auto &sender_state = next.announcers[pkt.sender];
    sender_state.last_seen_ms = r.now_ms();
    bool changed = false;
    if (pkt.ty == SEDS_DT_DISCOVERY_ANNOUNCE) {
        std::unordered_set<uint32_t> eps;
        for (uint32_t ep : decode_discovery_endpoints(pkt)) {
            eps.insert(ep);
        }
        changed = sender_state.endpoints != eps;
        sender_state.endpoints = std::move(eps);
    } else if (pkt.ty == SEDS_DT_DISCOVERY_TIMESYNC_SOURCES) {
        std::unordered_set<std::string> sources;
        for (auto& source : decode_discovery_timesync_sources(pkt)) {
            sources.insert(std::move(source));
        }
        changed = sender_state.timesync_sources != sources;
        sender_state.timesync_sources = std::move(sources);
    }
    next.endpoints.clear();
    next.timesync_sources.clear();
    next.last_seen_ms = 0;
    for (const auto &[_, sender] : next.announcers) {
        next.endpoints.insert(sender.endpoints.begin(), sender.endpoints.end());
        next.timesync_sources.insert(sender.timesync_sources.begin(), sender.timesync_sources.end());
        next.last_seen_ms = std::max(next.last_seen_ms, sender.last_seen_ms);
    }
    r.discovery_routes[*src_side] = std::move(next);
    if (changed) {
        note_discovery_topology_change(r, r.now_ms());
    }
}

}  // namespace seds
