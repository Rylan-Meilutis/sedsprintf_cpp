#include "internal.hpp"

namespace {

seds::RoutePolicy& policy_for(SedsRelay& r, int32_t src_side_id) {
    return src_side_id < 0 ? r.local_policy : r.source_policy[src_side_id];
}

void note_discovery_change_locked(SedsRelay& r, uint64_t now_ms) {
    r.discovery_interval_ms = seds::kDiscoveryFastMs;
    r.discovery_next_ms = now_ms;
}

void erase_tx_items_for_side_locked(SedsRelay& r, int32_t side_id) {
    for (auto it = r.tx_queue.begin(); it != r.tx_queue.end();) {
        if ((it->src_side && *it->src_side == side_id) || (it->dst_side && *it->dst_side == side_id)) {
            r.tx_queue_bytes -= seds::byte_cost(*it);
            it = r.tx_queue.erase(it);
        } else {
            ++it;
        }
    }
}

void erase_rx_items_for_side_locked(SedsRelay& r, int32_t side_id) {
    for (auto it = r.rx_queue.begin(); it != r.rx_queue.end();) {
        if (it->src_side && *it->src_side == side_id) {
            r.rx_queue_bytes -= seds::byte_cost(*it);
            it = r.rx_queue.erase(it);
        } else {
            ++it;
        }
    }
}

}  // namespace

extern "C" {

SedsRelay* seds_relay_new(SedsNowMsFn now_ms_cb, void* user) {
    auto relay = std::make_unique<SedsRelay>();
    relay->now_ms_cb = now_ms_cb;
    relay->clock_user = user;
    return relay.release();
}

void seds_relay_free(SedsRelay* r) { delete r; }

SedsResult seds_relay_announce_discovery(SedsRelay* r) {
    if (r == nullptr) return SEDS_BAD_ARG;
    std::scoped_lock lock(r->mu);
    seds::queue_discovery_packets(*r);
    return SEDS_OK;
}

SedsResult seds_relay_poll_discovery(SedsRelay* r, bool* out_did_queue) {
    if (r == nullptr) return SEDS_BAD_ARG;
    std::scoped_lock lock(r->mu);
    if (out_did_queue) *out_did_queue = false;
    if (r->now_ms() >= r->discovery_next_ms) {
        seds::queue_discovery_packets(*r);
        if (out_did_queue) *out_did_queue = true;
    }
    return SEDS_OK;
}

int32_t seds_relay_export_topology_len(SedsRelay* r) {
    if (r == nullptr) return SEDS_BAD_ARG;
    const auto json = seds::topology_snapshot_to_json(seds::export_topology_snapshot(*r));
    return static_cast<int32_t>(json.size() + 1u);
}

SedsResult seds_relay_export_topology(SedsRelay* r, char* buf, size_t buf_len) {
    if (r == nullptr) return SEDS_BAD_ARG;
    const auto json = seds::topology_snapshot_to_json(seds::export_topology_snapshot(*r));
    return static_cast<SedsResult>(seds::copy_text(json, buf, buf_len));
}

SedsResult seds_relay_periodic(SedsRelay* r, uint32_t timeout_ms) {
    bool did = false;
    seds_relay_poll_discovery(r, &did);
    return seds_relay_process_all_queues_with_timeout(r, timeout_ms);
}

int32_t seds_relay_add_side_serialized(SedsRelay* r, const char* name, size_t name_len, SedsTransmitFn tx, void* tx_user, bool reliable_enabled) {
    if (r == nullptr || tx == nullptr) return SEDS_BAD_ARG;
    std::scoped_lock lock(r->mu);
    r->sides.push_back({std::string(name ? name : "", name_len), tx, nullptr, tx_user, reliable_enabled, false});
    return static_cast<int32_t>(r->sides.size() - 1);
}

int32_t seds_relay_add_side_packet(SedsRelay* r, const char* name, size_t name_len, SedsEndpointHandlerFn tx, void* tx_user, bool reliable_enabled) {
    if (r == nullptr || tx == nullptr) return SEDS_BAD_ARG;
    std::scoped_lock lock(r->mu);
    r->sides.push_back({std::string(name ? name : "", name_len), nullptr, tx, tx_user, reliable_enabled, false});
    return static_cast<int32_t>(r->sides.size() - 1);
}

SedsResult seds_relay_remove_side(SedsRelay* r, int32_t side_id) {
    if (r == nullptr || side_id < 0 || static_cast<size_t>(side_id) >= r->sides.size()) return SEDS_INVALID_LINK_ID;
    std::scoped_lock lock(r->mu);
    const uint64_t now_ms = r->now_ms();
    r->sides[side_id].ingress_enabled = false;
    r->sides[side_id].egress_enabled = false;
    erase_tx_items_for_side_locked(*r, side_id);
    erase_rx_items_for_side_locked(*r, side_id);
    for (auto it = r->route_overrides.begin(); it != r->route_overrides.end();) {
        if (it->first.src_side == side_id || it->first.dst_side == side_id) {
            it = r->route_overrides.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = r->typed_route_overrides.begin(); it != r->typed_route_overrides.end();) {
        if (it->first.src_side == side_id || it->first.dst_side == side_id) {
            it = r->typed_route_overrides.erase(it);
        } else {
            ++it;
        }
    }
    r->source_policy.erase(side_id);
    r->local_policy.weights.erase(side_id);
    r->local_policy.priorities.erase(side_id);
    for (auto& [_, policy] : r->source_policy) {
        policy.weights.erase(side_id);
        policy.priorities.erase(side_id);
    }
    r->discovery_routes.erase(side_id);
    for (auto it = r->reliable_tx.begin(); it != r->reliable_tx.end();) {
        const auto reliable_side = static_cast<int32_t>(it->first >> 32u);
        if (reliable_side == side_id) {
            it = r->reliable_tx.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = r->reliable_rx.begin(); it != r->reliable_rx.end();) {
        const auto reliable_side = static_cast<int32_t>(it->first >> 32u);
        if (reliable_side == side_id) {
            it = r->reliable_rx.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = r->reliable_return_routes.begin(); it != r->reliable_return_routes.end();) {
        if (it->second.side == side_id) {
            it = r->reliable_return_routes.erase(it);
        } else {
            ++it;
        }
    }
    note_discovery_change_locked(*r, now_ms);
    return SEDS_OK;
}

SedsResult seds_relay_set_side_ingress_enabled(SedsRelay* r, int32_t side_id, bool enabled) {
    if (r == nullptr || side_id < 0 || static_cast<size_t>(side_id) >= r->sides.size()) return SEDS_INVALID_LINK_ID;
    std::scoped_lock lock(r->mu);
    r->sides[side_id].ingress_enabled = enabled;
    note_discovery_change_locked(*r, r->now_ms());
    return SEDS_OK;
}

SedsResult seds_relay_set_side_egress_enabled(SedsRelay* r, int32_t side_id, bool enabled) {
    if (r == nullptr || side_id < 0 || static_cast<size_t>(side_id) >= r->sides.size()) return SEDS_INVALID_LINK_ID;
    std::scoped_lock lock(r->mu);
    r->sides[side_id].egress_enabled = enabled;
    if (!enabled) {
        erase_tx_items_for_side_locked(*r, side_id);
    }
    note_discovery_change_locked(*r, r->now_ms());
    return SEDS_OK;
}

SedsResult seds_relay_set_side_link_local_enabled(SedsRelay* r, int32_t side_id, bool enabled) {
    if (r == nullptr || side_id < 0 || static_cast<size_t>(side_id) >= r->sides.size()) return SEDS_INVALID_LINK_ID;
    std::scoped_lock lock(r->mu);
    r->sides[side_id].link_local_enabled = enabled;
    note_discovery_change_locked(*r, r->now_ms());
    return SEDS_OK;
}

SedsResult seds_relay_set_route(SedsRelay* r, int32_t src_side_id, int32_t dst_side_id, bool enabled) {
    if (r == nullptr) return SEDS_BAD_ARG;
    std::scoped_lock lock(r->mu);
    r->route_overrides[{src_side_id, dst_side_id}] = enabled;
    return SEDS_OK;
}

SedsResult seds_relay_clear_route(SedsRelay* r, int32_t src_side_id, int32_t dst_side_id) {
    if (r == nullptr) return SEDS_BAD_ARG;
    std::scoped_lock lock(r->mu);
    r->route_overrides.erase({src_side_id, dst_side_id});
    return SEDS_OK;
}

SedsResult seds_relay_set_typed_route(SedsRelay* r, int32_t src_side_id, uint32_t ty, int32_t dst_side_id, bool enabled) {
    if (r == nullptr || !seds::valid_type(ty)) return SEDS_BAD_ARG;
    std::scoped_lock lock(r->mu);
    r->typed_route_overrides[{src_side_id, ty, dst_side_id}] = enabled;
    return SEDS_OK;
}

SedsResult seds_relay_clear_typed_route(SedsRelay* r, int32_t src_side_id, uint32_t ty, int32_t dst_side_id) {
    if (r == nullptr) return SEDS_BAD_ARG;
    std::scoped_lock lock(r->mu);
    r->typed_route_overrides.erase({src_side_id, ty, dst_side_id});
    return SEDS_OK;
}

SedsResult seds_relay_set_source_route_mode(SedsRelay* r, int32_t src_side_id, SedsRouteSelectionMode mode) {
    if (r == nullptr) return SEDS_BAD_ARG;
    std::scoped_lock lock(r->mu);
    policy_for(*r, src_side_id).mode = mode;
    return SEDS_OK;
}

SedsResult seds_relay_clear_source_route_mode(SedsRelay* r, int32_t src_side_id) { return seds_relay_set_source_route_mode(r, src_side_id, Seds_RSM_Fanout); }
SedsResult seds_relay_set_route_weight(SedsRelay* r, int32_t src_side_id, int32_t dst_side_id, uint32_t weight) { if (!r) return SEDS_BAD_ARG; std::scoped_lock lock(r->mu); policy_for(*r, src_side_id).weights[dst_side_id] = weight; return SEDS_OK; }
SedsResult seds_relay_clear_route_weight(SedsRelay* r, int32_t src_side_id, int32_t dst_side_id) { if (!r) return SEDS_BAD_ARG; std::scoped_lock lock(r->mu); policy_for(*r, src_side_id).weights.erase(dst_side_id); return SEDS_OK; }
SedsResult seds_relay_set_route_priority(SedsRelay* r, int32_t src_side_id, int32_t dst_side_id, uint32_t priority) { if (!r) return SEDS_BAD_ARG; std::scoped_lock lock(r->mu); policy_for(*r, src_side_id).priorities[dst_side_id] = priority; return SEDS_OK; }
SedsResult seds_relay_clear_route_priority(SedsRelay* r, int32_t src_side_id, int32_t dst_side_id) { if (!r) return SEDS_BAD_ARG; std::scoped_lock lock(r->mu); policy_for(*r, src_side_id).priorities.erase(dst_side_id); return SEDS_OK; }

SedsResult seds_relay_rx_serialized_from_side(SedsRelay* r, uint32_t side_id, const uint8_t* bytes, size_t len) {
    const auto frame = seds::peek_frame_info(bytes, len, true);
    if (r == nullptr || !frame) return SEDS_DESERIALIZE;
    std::scoped_lock lock(r->mu);
    if (static_cast<size_t>(side_id) >= r->sides.size()) return SEDS_INVALID_LINK_ID;
    if (!r->sides[side_id].ingress_enabled) return SEDS_INVALID_LINK_ID;
    if (!frame->reliable.has_value()) {
        if (const auto id = seds::packet_id_from_wire(bytes, len); id.has_value()) {
        const uint64_t dedupe_id = *id ^ (static_cast<uint64_t>(side_id) << 56u);
        if (r->recent_set.count(dedupe_id) != 0u) {
            return SEDS_OK;
        }
        seds::push_recent(*r, dedupe_id);
        }
    }
    const auto pkt = seds::deserialize_packet(bytes, len);
    if (!pkt) return SEDS_DESERIALIZE;
    if (!seds::process_reliable_ingress(*r, static_cast<int32_t>(side_id), *frame, *pkt,
                                        std::span<const uint8_t>(bytes, len))) {
        return SEDS_OK;
    }
    if (!seds::enqueue_rx(r->rx_queue, r->rx_queue_bytes, {*pkt, static_cast<int32_t>(side_id), {}})) {
        return SEDS_PACKET_TOO_LARGE;
    }
    for (auto& released : r->reliable_released_rx) {
        seds::enqueue_rx(r->rx_queue, r->rx_queue_bytes, std::move(released));
    }
    r->reliable_released_rx.clear();
    return SEDS_OK;
}

SedsResult seds_relay_rx_packet_from_side(SedsRelay* r, uint32_t side_id, const SedsPacketView* view) {
    seds::PacketData pkt;
    if (r == nullptr || !seds::packet_from_view(view, pkt)) return SEDS_BAD_ARG;
    std::scoped_lock lock(r->mu);
    if (static_cast<size_t>(side_id) >= r->sides.size()) return SEDS_INVALID_LINK_ID;
    if (!r->sides[side_id].ingress_enabled) return SEDS_INVALID_LINK_ID;
    return seds::enqueue_rx(r->rx_queue, r->rx_queue_bytes, {std::move(pkt), static_cast<int32_t>(side_id), {}})
               ? SEDS_OK
               : SEDS_PACKET_TOO_LARGE;
}

SedsResult seds_relay_process_rx_queue(SedsRelay* r) { return seds_relay_process_rx_queue_with_timeout(r, 0); }
SedsResult seds_relay_process_tx_queue(SedsRelay* r) { return seds_relay_process_tx_queue_with_timeout(r, 0); }
SedsResult seds_relay_process_all_queues(SedsRelay* r) { return seds_relay_process_all_queues_with_timeout(r, 0); }

SedsResult seds_relay_clear_queues(SedsRelay* r) {
    if (r == nullptr) return SEDS_BAD_ARG;
    std::scoped_lock lock(r->mu);
    seds::clear_tx_queue(r->tx_queue, r->tx_queue_bytes);
    seds::clear_rx_queue(r->rx_queue, r->rx_queue_bytes);
    return SEDS_OK;
}

SedsResult seds_relay_process_rx_queue_with_timeout(SedsRelay* r, uint32_t) {
    if (r == nullptr) return SEDS_BAD_ARG;
    std::scoped_lock lock(r->mu);
    while (auto item = seds::pop_rx(r->rx_queue, r->rx_queue_bytes)) {
        seds::relay_receive_impl(*r, std::move(item->pkt), item->src_side);
    }
    return SEDS_OK;
}

SedsResult seds_relay_process_tx_queue_with_timeout(SedsRelay* r, uint32_t) {
    if (r == nullptr) return SEDS_BAD_ARG;
    std::scoped_lock lock(r->mu);
    seds::process_reliable_timeouts(*r);
    while (auto item = seds::pop_tx(r->tx_queue, r->tx_queue_bytes)) {
        const auto rc = seds::transmit_item(*r, *item);
        if (rc != SEDS_OK) {
            if (rc == SEDS_IO) {
                seds::enqueue_tx_front(r->tx_queue, r->tx_queue_bytes, std::move(*item));
                return SEDS_OK;
            }
            return rc;
        }
    }
    return SEDS_OK;
}

SedsResult seds_relay_process_all_queues_with_timeout(SedsRelay* r, uint32_t timeout_ms) {
    if (r == nullptr) return SEDS_BAD_ARG;
    std::scoped_lock lock(r->mu);
    const uint64_t start_ms = r->now_ms();
    auto timeout_expired = [&](uint64_t now_ms) {
        return timeout_ms != 0 && static_cast<uint64_t>(now_ms - start_ms) >= timeout_ms;
    };
    do {
        seds::process_reliable_timeouts(*r);
        if (auto item = seds::pop_rx(r->rx_queue, r->rx_queue_bytes)) {
            seds::relay_receive_impl(*r, std::move(item->pkt), item->src_side);
        }
        if (!timeout_expired(r->now_ms())) {
            if (auto item = seds::pop_tx(r->tx_queue, r->tx_queue_bytes)) {
                const auto rc = seds::transmit_item(*r, *item);
                if (rc != SEDS_OK) {
                    if (rc == SEDS_IO) {
                        seds::enqueue_tx_front(r->tx_queue, r->tx_queue_bytes, std::move(*item));
                        return SEDS_OK;
                    }
                    return rc;
                }
            }
        }
    } while ((!r->rx_queue.empty() || !r->tx_queue.empty()) && !timeout_expired(r->now_ms()));
    return SEDS_OK;
}

}  // extern "C"
