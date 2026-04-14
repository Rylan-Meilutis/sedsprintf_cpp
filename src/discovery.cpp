#include "internal.hpp"

#include <algorithm>
#include <cstring>
#include <ranges>
#include <type_traits>

namespace seds {
namespace {

template <typename OwnerT>
std::vector<std::string> local_discovery_timesync_sources(const OwnerT& owner) {
    std::vector<std::string> out;
    if constexpr (std::is_same_v<OwnerT, SedsRouter>) {
        if (owner.timesync.enabled && owner.timesync.has_network_time) {
            out.push_back(owner.timesync.current_source.empty() ? "local" : owner.timesync.current_source);
        }
    }
    return out;
}

template <typename SenderStateT>
void refresh_sender_topology_state(SenderStateT& sender_state) {
    normalize_topology_boards(sender_state.topology_boards);
    const auto [reachable, sources] = summarize_topology_boards(sender_state.topology_boards);
    sender_state.endpoints.clear();
    sender_state.timesync_sources.clear();
    sender_state.endpoints.insert(reachable.begin(), reachable.end());
    sender_state.timesync_sources.insert(sources.begin(), sources.end());
}

void recompute_discovery_side_state(DiscoveryRoute& route) {
    route.endpoints.clear();
    route.timesync_sources.clear();
    route.last_seen_ms = 0;
    for (const auto& [_, sender] : route.announcers) {
        route.endpoints.insert(sender.endpoints.begin(), sender.endpoints.end());
        route.timesync_sources.insert(sender.timesync_sources.begin(), sender.timesync_sources.end());
        route.last_seen_ms = std::max(route.last_seen_ms, sender.last_seen_ms);
    }
}

TopologyBoardNode* sender_topology_board_mut(DiscoveryRoute::SenderState& sender_state, const std::string& sender_id) {
    auto it = std::ranges::find(sender_state.topology_boards, sender_id, &TopologyBoardNode::sender_id);
    if (it != sender_state.topology_boards.end()) {
        return &*it;
    }
    sender_state.topology_boards.push_back(
        TopologyBoardNode{sender_id, {}, {}, {}});
    return &sender_state.topology_boards.back();
}

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
        recompute_discovery_side_state(route);
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

std::vector<std::string> decode_discovery_timesync_sources_packet(const PacketData& pkt) {
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

std::vector<TopologyBoardNode> decode_discovery_topology_packet(const PacketData& pkt) {
    try {
        return decode_discovery_topology_payload(pkt.payload);
    } catch (const std::invalid_argument&) {
        return {};
    }
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

TopologyBoardNode local_router_topology_board(const SedsRouter& r, uint64_t now_ms, bool link_local_enabled) {
    std::vector<std::string> connections;
    for (const auto& [_, route] : r.discovery_routes) {
        if (now_ms - route.last_seen_ms > kDiscoveryTtlMs) {
            continue;
        }
        for (const auto& [sender, sender_state] : route.announcers) {
            if (now_ms - sender_state.last_seen_ms <= kDiscoveryTtlMs) {
                connections.push_back(sender);
            }
        }
    }
    sort_dedup_strings(connections);
    return TopologyBoardNode{
        r.sender,
        router_local_discovery_endpoints(r, link_local_enabled),
        local_discovery_timesync_sources(r),
        std::move(connections),
    };
}

TopologyBoardNode local_relay_topology_board(const SedsRelay& r, uint64_t now_ms) {
    std::vector<std::string> connections;
    for (const auto& [_, route] : r.discovery_routes) {
        if (now_ms - route.last_seen_ms > kDiscoveryTtlMs) {
            continue;
        }
        for (const auto& [sender, sender_state] : route.announcers) {
            if (now_ms - sender_state.last_seen_ms <= kDiscoveryTtlMs) {
                connections.push_back(sender);
            }
        }
    }
    sort_dedup_strings(connections);
    return TopologyBoardNode{"RELAY", {}, {}, std::move(connections)};
}

template <typename OwnerT>
std::vector<TopologyBoardNode> advertised_discovery_topology_for_side_impl(const OwnerT& owner, int32_t dst_side) {
    const uint64_t now_ms = owner.now_ms();
    const bool link_local_enabled =
        dst_side >= 0 && static_cast<size_t>(dst_side) < owner.sides.size() && owner.sides[dst_side].link_local_enabled;
    std::vector<TopologyBoardNode> boards;
    if constexpr (std::is_same_v<OwnerT, SedsRouter>) {
        boards.push_back(local_router_topology_board(owner, now_ms, link_local_enabled));
    } else {
        boards.push_back(local_relay_topology_board(owner, now_ms));
    }
    std::string local_sender;
    if constexpr (std::is_same_v<OwnerT, SedsRouter>) {
        local_sender = owner.sender;
    } else {
        local_sender = "RELAY";
    }
    for (const auto& [_, route] : owner.discovery_routes) {
        if (now_ms - route.last_seen_ms > kDiscoveryTtlMs) {
            continue;
        }
        for (const auto& [announcer, sender_state] : route.announcers) {
            if (now_ms - sender_state.last_seen_ms > kDiscoveryTtlMs) {
                continue;
            }
            auto sender_boards = sender_state.topology_boards;
            if (sender_boards.empty()) {
                sender_boards.push_back(
                    TopologyBoardNode{announcer,
                                      std::vector<uint32_t>(sender_state.endpoints.begin(), sender_state.endpoints.end()),
                                      std::vector<std::string>(sender_state.timesync_sources.begin(),
                                                               sender_state.timesync_sources.end()),
                                      {local_sender}});
            } else {
                auto board_it = std::ranges::find(sender_boards, announcer, &TopologyBoardNode::sender_id);
                if (board_it != sender_boards.end()) {
                    board_it->connections.push_back(local_sender);
                }
            }
            if (!link_local_enabled) {
                for (auto& board : sender_boards) {
                    board.reachable_endpoints.erase(
                        std::remove_if(board.reachable_endpoints.begin(),
                                       board.reachable_endpoints.end(),
                                       [](const uint32_t ep) { return endpoint_link_local_only(ep); }),
                        board.reachable_endpoints.end());
                }
            }
            merge_topology_boards(boards, sender_boards);
        }
    }
    normalize_topology_boards(boards);
    return boards;
}

template <typename OwnerT>
std::vector<uint32_t> advertised_endpoints_for_side(const OwnerT& owner, int32_t dst_side) {
    const auto boards = advertised_discovery_topology_for_side_impl(owner, dst_side);
    auto [reachable_endpoints, _] = summarize_topology_boards(boards);
    const bool link_local_enabled =
        dst_side >= 0 && static_cast<size_t>(dst_side) < owner.sides.size() && owner.sides[dst_side].link_local_enabled;
    reachable_endpoints.erase(std::remove_if(reachable_endpoints.begin(),
                                             reachable_endpoints.end(),
                                             [link_local_enabled](const uint32_t ep) {
                                                 return ep == SEDS_EP_DISCOVERY ||
                                                        (!link_local_enabled && endpoint_link_local_only(ep));
                                             }),
                              reachable_endpoints.end());
    return reachable_endpoints;
}

template <typename OwnerT>
std::vector<std::string> advertised_timesync_sources_for_side(const OwnerT& owner, int32_t dst_side) {
    auto [_, sources] = summarize_topology_boards(advertised_discovery_topology_for_side_impl(owner, dst_side));
    return sources;
}

}  // namespace

std::vector<TopologyBoardNode> advertised_discovery_topology_for_side(const SedsRouter& r, int32_t dst_side) {
    return advertised_discovery_topology_for_side_impl(r, dst_side);
}

std::vector<TopologyBoardNode> advertised_discovery_topology_for_side(const SedsRelay& r, int32_t dst_side) {
    return advertised_discovery_topology_for_side_impl(r, dst_side);
}

void queue_discovery_packets(SedsRouter& r) {
    const uint64_t now = r.now_ms();
    if (prune_discovery_routes(r.discovery_routes, now)) {
        note_discovery_topology_change(r, now);
    }
    for (size_t side_id = 0; side_id < r.sides.size(); ++side_id) {
        if (!r.sides[side_id].egress_enabled) {
            continue;
        }
        const auto topology = advertised_discovery_topology_for_side_impl(r, static_cast<int32_t>(side_id));
        std::optional<PacketData> topology_pkt;
        if (!topology.empty()) {
            const auto topo = build_discovery_topology(r.sender, now, topology);
            topology_pkt = PacketData{topo.type(), topo.sender(), topo.endpoints(), topo.timestamp(),
                                      std::vector<uint8_t>(topo.payload().begin(), topo.payload().end())};
        }
        auto timesync_sources = advertised_timesync_sources_for_side(r, static_cast<int32_t>(side_id));
        if (topology_pkt.has_value()) {
            enqueue_tx_front(r.tx_queue, r.tx_queue_bytes,
                             {std::move(*topology_pkt), std::nullopt, static_cast<int32_t>(side_id), false});
        }
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
        for (const uint32_t ep : advertised_endpoints_for_side(r, static_cast<int32_t>(side_id))) {
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
        const auto topology = advertised_discovery_topology_for_side_impl(r, static_cast<int32_t>(side_id));
        std::optional<PacketData> topology_pkt;
        if (!topology.empty()) {
            const auto topo = build_discovery_topology("RELAY", now, topology);
            topology_pkt = PacketData{topo.type(), topo.sender(), topo.endpoints(), topo.timestamp(),
                                      std::vector<uint8_t>(topo.payload().begin(), topo.payload().end())};
        }
        auto timesync_sources = advertised_timesync_sources_for_side(r, static_cast<int32_t>(side_id));
        if (topology_pkt.has_value()) {
            enqueue_tx_front(r.tx_queue, r.tx_queue_bytes,
                             {std::move(*topology_pkt), std::nullopt, static_cast<int32_t>(side_id), false});
        }
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
        for (const uint32_t ep : advertised_endpoints_for_side(r, static_cast<int32_t>(side_id))) {
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
    const bool link_local_enabled =
        *src_side >= 0 && static_cast<size_t>(*src_side) < r.sides.size() && r.sides[*src_side].link_local_enabled;
    bool changed = false;
    if (pkt.ty == SEDS_DT_DISCOVERY_ANNOUNCE) {
        auto reachable = decode_discovery_endpoints(pkt);
        auto* board = sender_topology_board_mut(sender_state, pkt.sender);
        changed = board->reachable_endpoints != reachable;
        board->reachable_endpoints = std::move(reachable);
        if (!link_local_enabled) {
            board->reachable_endpoints.erase(
                std::remove_if(board->reachable_endpoints.begin(),
                               board->reachable_endpoints.end(),
                               [](const uint32_t ep) { return endpoint_link_local_only(ep); }),
                board->reachable_endpoints.end());
        }
        refresh_sender_topology_state(sender_state);
    } else if (pkt.ty == SEDS_DT_DISCOVERY_TIMESYNC_SOURCES) {
        auto* board = sender_topology_board_mut(sender_state, pkt.sender);
        const auto sources = decode_discovery_timesync_sources_packet(pkt);
        changed = board->reachable_timesync_sources != sources;
        board->reachable_timesync_sources = sources;
        refresh_sender_topology_state(sender_state);
    } else if (pkt.ty == SEDS_DT_DISCOVERY_TOPOLOGY) {
        auto boards = decode_discovery_topology_packet(pkt);
        if (!link_local_enabled) {
            for (auto& board : boards) {
                board.reachable_endpoints.erase(
                    std::remove_if(board.reachable_endpoints.begin(),
                                   board.reachable_endpoints.end(),
                                   [](const uint32_t ep) { return endpoint_link_local_only(ep); }),
                    board.reachable_endpoints.end());
            }
        }
        changed = sender_state.topology_boards != boards;
        sender_state.topology_boards = std::move(boards);
        refresh_sender_topology_state(sender_state);
    }
    sender_state.last_seen_ms = r.now_ms();
    recompute_discovery_side_state(next);
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
    const bool link_local_enabled =
        *src_side >= 0 && static_cast<size_t>(*src_side) < r.sides.size() && r.sides[*src_side].link_local_enabled;
    bool changed = false;
    if (pkt.ty == SEDS_DT_DISCOVERY_ANNOUNCE) {
        auto reachable = decode_discovery_endpoints(pkt);
        auto* board = sender_topology_board_mut(sender_state, pkt.sender);
        changed = board->reachable_endpoints != reachable;
        board->reachable_endpoints = std::move(reachable);
        if (!link_local_enabled) {
            board->reachable_endpoints.erase(
                std::remove_if(board->reachable_endpoints.begin(),
                               board->reachable_endpoints.end(),
                               [](const uint32_t ep) { return endpoint_link_local_only(ep); }),
                board->reachable_endpoints.end());
        }
        refresh_sender_topology_state(sender_state);
    } else if (pkt.ty == SEDS_DT_DISCOVERY_TIMESYNC_SOURCES) {
        auto* board = sender_topology_board_mut(sender_state, pkt.sender);
        const auto sources = decode_discovery_timesync_sources_packet(pkt);
        changed = board->reachable_timesync_sources != sources;
        board->reachable_timesync_sources = sources;
        refresh_sender_topology_state(sender_state);
    } else if (pkt.ty == SEDS_DT_DISCOVERY_TOPOLOGY) {
        auto boards = decode_discovery_topology_packet(pkt);
        if (!link_local_enabled) {
            for (auto& board : boards) {
                board.reachable_endpoints.erase(
                    std::remove_if(board.reachable_endpoints.begin(),
                                   board.reachable_endpoints.end(),
                                   [](const uint32_t ep) { return endpoint_link_local_only(ep); }),
                    board.reachable_endpoints.end());
            }
        }
        changed = sender_state.topology_boards != boards;
        sender_state.topology_boards = std::move(boards);
        refresh_sender_topology_state(sender_state);
    }
    sender_state.last_seen_ms = r.now_ms();
    recompute_discovery_side_state(next);
    r.discovery_routes[*src_side] = std::move(next);
    if (changed) {
        note_discovery_topology_change(r, r.now_ms());
    }
}

}  // namespace seds
