#include "internal.hpp"

namespace
{
  seds::RoutePolicy & policy_for(SedsRouter & r, int32_t src_side_id)
  {
    return src_side_id < 0 ? r.local_policy : r.source_policy[src_side_id];
  }

  bool timeout_expired(uint64_t start_ms, uint64_t now_ms, uint32_t timeout_ms)
  {
    if (timeout_ms == 0)
    {
      return false;
    }
    return static_cast<uint64_t>(now_ms - start_ms) >= timeout_ms;
  }

  void note_discovery_change_locked(SedsRouter & r, uint64_t now_ms)
  {
    r.discovery_interval_ms = seds::kDiscoveryFastMs;
    r.discovery_next_ms = now_ms;
  }

  void erase_tx_items_for_side_locked(SedsRouter & r, int32_t side_id)
  {
    for (auto it = r.tx_queue.begin(); it != r.tx_queue.end();)
    {
      if ((it->src_side && *it->src_side == side_id) || (it->dst_side && *it->dst_side == side_id))
      {
        r.tx_queue_bytes -= seds::byte_cost(*it);
        it = r.tx_queue.erase(it);
      }
      else
      {
        ++it;
      }
    }
  }

  void erase_rx_items_for_side_locked(SedsRouter & r, int32_t side_id)
  {
    for (auto it = r.rx_queue.begin(); it != r.rx_queue.end();)
    {
      if (it->src_side && *it->src_side == side_id)
      {
        r.rx_queue_bytes -= seds::byte_cost(*it);
        it = r.rx_queue.erase(it);
      }
      else
      {
        ++it;
      }
    }
  }

  bool all_endpoints_local(const SedsRouter & r, const seds::PacketData & pkt)
  {
    if (pkt.endpoints.empty())
    {
      return false;
    }
    for (const uint32_t endpoint: pkt.endpoints)
    {
      bool found = false;
      for (const auto & local: r.locals)
      {
        if (local.endpoint == endpoint && (local.packet_handler != nullptr || local.serialized_handler != nullptr))
        {
          found = true;
          break;
        }
      }
      if (!found)
      {
        return false;
      }
    }
    return true;
  }

  SedsResult set_network_time_from_fields(SedsRouter & r, bool has_year, int32_t year, bool has_month, uint8_t month,
                                          bool has_day, uint8_t day, bool has_hour, uint8_t hour, bool has_minute,
                                          uint8_t minute, bool has_second, uint8_t second, bool has_nanosecond,
                                          uint32_t nanosecond)
  {
    SedsNetworkTime current{};
    if (r.timesync.has_network_time)
    {
      seds::fill_network_time(r.current_network_ms(), current);
    }
    else
    {
      current.has_year = true;
      current.year = 1970;
      current.has_month = true;
      current.month = 1;
      current.has_day = true;
      current.day = 1;
      current.has_hour = true;
      current.hour = 0;
      current.has_minute = true;
      current.minute = 0;
      current.has_second = true;
      current.second = 0;
      current.has_nanosecond = true;
      current.nanosecond = 0;
    }

    if (has_year)
      current.year = year;
    if (has_month)
      current.month = month;
    if (has_day)
      current.day = day;
    if (has_hour)
      current.hour = hour;
    if (has_minute)
      current.minute = minute;
    if (has_second)
      current.second = second;
    if (has_nanosecond)
      current.nanosecond = nanosecond;

    const auto unix_ms = seds::network_time_to_unix_ms(current.year, current.month, current.day, current.hour,
                                                       current.minute, current.second, current.nanosecond);
    if (!unix_ms.has_value())
    {
      return SEDS_BAD_ARG;
    }

    r.timesync.has_network_time = true;
    r.timesync.network_anchor_local_ms = r.now_ms();
    r.timesync.network_anchor_unix_ms = *unix_ms;
    if (r.timesync.current_source.empty())
    {
      r.timesync.current_source = "local";
      r.timesync.current_source_priority = r.timesync.priority;
    }
    return SEDS_OK;
  }
} // namespace

extern "C" {
SedsRouter * seds_router_new(SedsRouterMode mode, SedsNowMsFn now_ms_cb, void * user,
                             const SedsLocalEndpointDesc * handlers, size_t n_handlers)
{
  auto router = std::make_unique<SedsRouter>(mode);
  router->now_ms_cb = now_ms_cb;
  router->clock_user = user;
  for (size_t i = 0; i < n_handlers; ++i)
  {
    if (handlers[i].endpoint == SEDS_EP_DISCOVERY || handlers[i].endpoint == SEDS_EP_TIME_SYNC)
    {
      return nullptr;
    }
    router->locals.push_back(
      {handlers[i].endpoint, handlers[i].packet_handler, handlers[i].serialized_handler, handlers[i].user});
  }
  return router.release();
}

void seds_router_free(SedsRouter * r) { delete r; }

SedsResult seds_router_set_sender(SedsRouter * r, const char * sender, size_t sender_len)
{
  if (r == nullptr || sender == nullptr)
  {
    return SEDS_BAD_ARG;
  }
  std::scoped_lock lock(r->mu);
  r->sender.assign(sender, sender_len);
  r->node_sender = r->sender;
  return SEDS_OK;
}

SedsResult seds_router_configure_timesync(SedsRouter * r, bool enabled, uint32_t role, uint64_t priority,
                                          uint64_t source_timeout_ms, uint64_t announce_interval_ms,
                                          uint64_t request_interval_ms)
{
  if (r == nullptr)
    return SEDS_BAD_ARG;
  std::scoped_lock lock(r->mu);
  r->timesync.enabled = enabled;
  r->timesync.role = role;
  r->timesync.priority = priority;
  r->timesync.source_timeout_ms = source_timeout_ms;
  r->timesync.announce_interval_ms = announce_interval_ms;
  r->timesync.request_interval_ms = request_interval_ms;
  if (enabled && role == 1u && !r->timesync.has_network_time)
  {
    r->timesync.has_network_time = true;
    r->timesync.network_anchor_local_ms = r->now_ms();
    r->timesync.network_anchor_unix_ms = 1'700'000'000'000ull + r->timesync.network_anchor_local_ms;
  }
  return SEDS_OK;
}

SedsResult seds_router_get_network_time_ms(SedsRouter * r, uint64_t * out_ms)
{
  if (r == nullptr || out_ms == nullptr)
    return SEDS_BAD_ARG;
  std::scoped_lock lock(r->mu);
  if (!r->timesync.has_network_time)
    return SEDS_ERR;
  if (!r->timesync.current_source.empty())
  {
    const uint64_t now = r->now_ms();
    auto it = r->timesync.sources.find(r->timesync.current_source);
    if (it == r->timesync.sources.end() || now - it->second.last_seen_ms > r->timesync.source_timeout_ms)
    {
      std::string best_name;
      uint64_t best_pri = UINT64_MAX;
      uint64_t best_seen = 0;
      uint64_t best_time = 0;
      for (const auto & [name, src]: r->timesync.sources)
      {
        if (now - src.last_seen_ms > r->timesync.source_timeout_ms)
        {
          continue;
        }
        if (src.priority < best_pri || (src.priority == best_pri && src.last_seen_ms > best_seen))
        {
          best_pri = src.priority;
          best_seen = src.last_seen_ms;
          best_time = src.last_time_ms;
          best_name = name;
        }
      }
      if (!best_name.empty())
      {
        const uint64_t candidate = best_time + (now - best_seen);
        const uint64_t current = r->current_network_ms();
        r->timesync.current_source = best_name;
        r->timesync.current_source_priority = best_pri;
        r->timesync.network_anchor_local_ms = now;
        r->timesync.network_anchor_unix_ms = candidate < current ? current : candidate;
      }
    }
  }
  *out_ms = r->current_network_ms();
  return SEDS_OK;
}

SedsResult seds_router_get_network_time(SedsRouter * r, SedsNetworkTime * out)
{
  if (r == nullptr || out == nullptr)
    return SEDS_BAD_ARG;
  uint64_t ms = 0;
  const auto rc = seds_router_get_network_time_ms(r, &ms);
  if (rc != SEDS_OK)
    return rc;
  seds::fill_network_time(ms, *out);
  return SEDS_OK;
}

SedsResult seds_router_poll_timesync(SedsRouter * r, bool * out_did_queue)
{
  if (r == nullptr)
    return SEDS_BAD_ARG;
  std::scoped_lock lock(r->mu);
  if (out_did_queue)
    *out_did_queue = false;
  if (!r->timesync.enabled)
    return SEDS_OK;
  const uint64_t now = r->now_ms();
  for (auto it = r->timesync.sources.begin(); it != r->timesync.sources.end();)
  {
    if (now - it->second.last_seen_ms > r->timesync.source_timeout_ms)
    {
      if (it->first == r->timesync.current_source)
      {
        r->timesync.current_source.clear();
        r->timesync.current_source_priority = UINT64_MAX;
      }
      it = r->timesync.sources.erase(it);
    }
    else
    {
      ++it;
    }
  }
  if ((r->timesync.role == 1u || (r->timesync.role == 2u && r->timesync.sources.empty())) &&
      now >= r->timesync.last_announce_ms + r->timesync.announce_interval_ms)
  {
    std::vector<uint8_t> payload;
    seds::append_le<uint64_t>(r->timesync.priority, payload);
    seds::append_le<uint64_t>(r->current_network_ms(), payload);
    auto pkt = seds::make_internal_packet(SEDS_DT_TIME_SYNC_ANNOUNCE, now, std::move(payload));
    pkt.sender = r->sender;
    seds::enqueue_tx_front(r->tx_queue, r->tx_queue_bytes,
                           {std::move(pkt), std::nullopt, std::nullopt, false});
    r->timesync.last_announce_ms = now;
    if (out_did_queue)
      *out_did_queue = true;
  }
  else if (r->timesync.role == 0u && now >= r->timesync.last_request_ms + r->timesync.request_interval_ms)
  {
    if (r->timesync.current_source.empty())
    {
      for (const auto & [_, route]: r->discovery_routes)
      {
        if (!route.timesync_sources.empty())
        {
          r->timesync.current_source = *route.timesync_sources.begin();
          break;
        }
      }
    }
    if (r->timesync.current_source.empty())
    {
      return SEDS_OK;
    }
    std::vector<uint8_t> payload;
    seds::append_le<uint64_t>(now, payload);
    seds::append_le<uint64_t>(now, payload);
    auto pkt = seds::make_internal_packet(SEDS_DT_TIME_SYNC_REQUEST, now, std::move(payload));
    pkt.sender = r->sender;
    seds::enqueue_tx_front(r->tx_queue, r->tx_queue_bytes,
                           {std::move(pkt), std::nullopt, std::nullopt, false});
    r->timesync.last_request_ms = now;
    if (out_did_queue)
      *out_did_queue = true;
  }
  return SEDS_OK;
}

SedsResult seds_router_announce_discovery(SedsRouter * r)
{
  if (r == nullptr)
    return SEDS_BAD_ARG;
  std::scoped_lock lock(r->mu);
  seds::queue_discovery_packets(*r);
  return SEDS_OK;
}

SedsResult seds_router_poll_discovery(SedsRouter * r, bool * out_did_queue)
{
  if (r == nullptr)
    return SEDS_BAD_ARG;
  std::scoped_lock lock(r->mu);
  if (out_did_queue)
    *out_did_queue = false;
  if (r->now_ms() >= r->discovery_next_ms)
  {
    seds::queue_discovery_packets(*r);
    if (out_did_queue)
      *out_did_queue = true;
  }
  return SEDS_OK;
}

int32_t seds_router_export_topology_len(SedsRouter * r)
{
  if (r == nullptr)
    return SEDS_BAD_ARG;
  const auto json = seds::topology_snapshot_to_json(seds::export_topology_snapshot(*r));
  return static_cast<int32_t>(json.size() + 1u);
}

SedsResult seds_router_export_topology(SedsRouter * r, char * buf, size_t buf_len)
{
  if (r == nullptr)
    return SEDS_BAD_ARG;
  const auto json = seds::topology_snapshot_to_json(seds::export_topology_snapshot(*r));
  return static_cast<SedsResult>(seds::copy_text(json, buf, buf_len));
}

SedsResult seds_router_periodic(SedsRouter * r, uint32_t timeout_ms)
{
  bool did = false;
  seds_router_poll_timesync(r, &did);
  seds_router_poll_discovery(r, &did);
  return seds_router_process_all_queues_with_timeout(r, timeout_ms);
}

SedsResult seds_router_periodic_no_timesync(SedsRouter * r, uint32_t timeout_ms)
{
  bool did = false;
  seds_router_poll_discovery(r, &did);
  return seds_router_process_all_queues_with_timeout(r, timeout_ms);
}

SedsResult seds_router_set_local_network_time(SedsRouter * r, bool has_year, int32_t year, bool has_month,
                                              uint8_t month,
                                              bool has_day, uint8_t day, bool has_hour, uint8_t hour, bool has_minute,
                                              uint8_t minute, bool has_second, uint8_t second, bool has_nanosecond,
                                              uint32_t nanosecond)
{
  if (r == nullptr)
    return SEDS_BAD_ARG;
  std::scoped_lock lock(r->mu);
  return set_network_time_from_fields(*r, has_year, year, has_month, month, has_day, day, has_hour, hour, has_minute,
                                      minute, has_second, second, has_nanosecond, nanosecond);
}

SedsResult seds_router_set_local_network_date(SedsRouter * r, int32_t year, uint8_t month, uint8_t day)
{
  return seds_router_set_local_network_time(r, true, year, true, month, true, day, false, 0, false, 0, false, 0, false,
                                            0);
}

SedsResult seds_router_set_local_network_time_hm(SedsRouter * r, uint8_t hour, uint8_t minute)
{
  return seds_router_set_local_network_time(r, false, 0, false, 0, false, 0, true, hour, true, minute, false, 0, false,
                                            0);
}

SedsResult seds_router_set_local_network_time_hms(SedsRouter * r, uint8_t hour, uint8_t minute, uint8_t second)
{
  return seds_router_set_local_network_time(r, false, 0, false, 0, false, 0, true, hour, true, minute, true, second,
                                            false, 0);
}

SedsResult seds_router_set_local_network_time_hms_millis(SedsRouter * r, uint8_t hour, uint8_t minute, uint8_t second,
                                                         uint16_t millisecond)
{
  return seds_router_set_local_network_time(r, false, 0, false, 0, false, 0, true, hour, true, minute, true, second,
                                            true, static_cast<uint32_t>(millisecond) * 1000000u);
}

SedsResult seds_router_set_local_network_time_hms_nanos(SedsRouter * r, uint8_t hour, uint8_t minute, uint8_t second,
                                                        uint32_t nanosecond)
{
  return seds_router_set_local_network_time(r, false, 0, false, 0, false, 0, true, hour, true, minute, true, second,
                                            true, nanosecond);
}

SedsResult seds_router_set_local_network_datetime(SedsRouter * r, int32_t year, uint8_t month, uint8_t day,
                                                  uint8_t hour,
                                                  uint8_t minute, uint8_t second)
{
  return seds_router_set_local_network_time(r, true, year, true, month, true, day, true, hour, true, minute, true,
                                            second, false, 0);
}

SedsResult seds_router_set_local_network_datetime_millis(SedsRouter * r, int32_t year, uint8_t month, uint8_t day,
                                                         uint8_t hour, uint8_t minute, uint8_t second,
                                                         uint16_t millisecond)
{
  return seds_router_set_local_network_time(r, true, year, true, month, true, day, true, hour, true, minute, true,
                                            second, true, static_cast<uint32_t>(millisecond) * 1000000u);
}

SedsResult seds_router_set_local_network_datetime_nanos(SedsRouter * r, int32_t year, uint8_t month, uint8_t day,
                                                        uint8_t hour, uint8_t minute, uint8_t second,
                                                        uint32_t nanosecond)
{
  return seds_router_set_local_network_time(r, true, year, true, month, true, day, true, hour, true, minute, true,
                                            second, true, nanosecond);
}

int32_t seds_router_add_side_serialized(SedsRouter * r, const char * name, size_t name_len, SedsTransmitFn tx,
                                        void * tx_user, bool reliable_enabled)
{
  if (r == nullptr || tx == nullptr)
    return SEDS_BAD_ARG;
  std::scoped_lock lock(r->mu);
  r->sides.push_back({std::string(name ? name : "", name_len), tx, nullptr, tx_user, reliable_enabled, false});
  return static_cast<int32_t>(r->sides.size() - 1);
}

int32_t seds_router_add_side_packet(SedsRouter * r, const char * name, size_t name_len, SedsEndpointHandlerFn tx,
                                    void * tx_user, bool reliable_enabled)
{
  if (r == nullptr || tx == nullptr)
    return SEDS_BAD_ARG;
  std::scoped_lock lock(r->mu);
  r->sides.push_back({std::string(name ? name : "", name_len), nullptr, tx, tx_user, reliable_enabled, false});
  return static_cast<int32_t>(r->sides.size() - 1);
}

SedsResult seds_router_remove_side(SedsRouter * r, int32_t side_id)
{
  if (r == nullptr || side_id < 0 || static_cast<size_t>(side_id) >= r->sides.size())
    return SEDS_INVALID_LINK_ID;
  std::scoped_lock lock(r->mu);
  const uint64_t now_ms = r->now_ms();
  r->sides[side_id].ingress_enabled = false;
  r->sides[side_id].egress_enabled = false;
  erase_tx_items_for_side_locked(*r, side_id);
  erase_rx_items_for_side_locked(*r, side_id);
  for (auto it = r->route_overrides.begin(); it != r->route_overrides.end();)
  {
    if (it->first.src_side == side_id || it->first.dst_side == side_id)
    {
      it = r->route_overrides.erase(it);
    }
    else
    {
      ++it;
    }
  }
  for (auto it = r->typed_route_overrides.begin(); it != r->typed_route_overrides.end();)
  {
    if (it->first.src_side == side_id || it->first.dst_side == side_id)
    {
      it = r->typed_route_overrides.erase(it);
    }
    else
    {
      ++it;
    }
  }
  r->source_policy.erase(side_id);
  r->local_policy.weights.erase(side_id);
  r->local_policy.priorities.erase(side_id);
  for (auto & [_, policy]: r->source_policy)
  {
    policy.weights.erase(side_id);
    policy.priorities.erase(side_id);
  }
  r->discovery_routes.erase(side_id);
  for (auto it = r->reliable_tx.begin(); it != r->reliable_tx.end();)
  {
    const auto reliable_side = static_cast<int32_t>(it->first >> 32u);
    if (reliable_side == side_id)
    {
      it = r->reliable_tx.erase(it);
    }
    else
    {
      ++it;
    }
  }
  for (auto it = r->reliable_rx.begin(); it != r->reliable_rx.end();)
  {
    const auto reliable_side = static_cast<int32_t>(it->first >> 32u);
    if (reliable_side == side_id)
    {
      it = r->reliable_rx.erase(it);
    }
    else
    {
      ++it;
    }
  }
  for (auto it = r->reliable_return_routes.begin(); it != r->reliable_return_routes.end();)
  {
    if (it->second.side == side_id)
    {
      it = r->reliable_return_routes.erase(it);
    }
    else
    {
      ++it;
    }
  }
  note_discovery_change_locked(*r, now_ms);
  return SEDS_OK;
}

SedsResult seds_router_set_side_ingress_enabled(SedsRouter * r, int32_t side_id, bool enabled)
{
  if (r == nullptr || side_id < 0 || static_cast<size_t>(side_id) >= r->sides.size())
    return SEDS_INVALID_LINK_ID;
  std::scoped_lock lock(r->mu);
  r->sides[side_id].ingress_enabled = enabled;
  note_discovery_change_locked(*r, r->now_ms());
  return SEDS_OK;
}

SedsResult seds_router_set_side_egress_enabled(SedsRouter * r, int32_t side_id, bool enabled)
{
  if (r == nullptr || side_id < 0 || static_cast<size_t>(side_id) >= r->sides.size())
    return SEDS_INVALID_LINK_ID;
  std::scoped_lock lock(r->mu);
  r->sides[side_id].egress_enabled = enabled;
  if (!enabled)
  {
    erase_tx_items_for_side_locked(*r, side_id);
  }
  note_discovery_change_locked(*r, r->now_ms());
  return SEDS_OK;
}

SedsResult seds_router_set_side_link_local_enabled(SedsRouter * r, int32_t side_id, bool enabled)
{
  if (r == nullptr || side_id < 0 || static_cast<size_t>(side_id) >= r->sides.size())
    return SEDS_INVALID_LINK_ID;
  std::scoped_lock lock(r->mu);
  r->sides[side_id].link_local_enabled = enabled;
  note_discovery_change_locked(*r, r->now_ms());
  return SEDS_OK;
}

SedsResult seds_router_set_route(SedsRouter * r, int32_t src_side_id, int32_t dst_side_id, bool enabled)
{
  if (r == nullptr)
    return SEDS_BAD_ARG;
  std::scoped_lock lock(r->mu);
  r->route_overrides[{src_side_id, dst_side_id}] = enabled;
  return SEDS_OK;
}

SedsResult seds_router_clear_route(SedsRouter * r, int32_t src_side_id, int32_t dst_side_id)
{
  if (r == nullptr)
    return SEDS_BAD_ARG;
  std::scoped_lock lock(r->mu);
  r->route_overrides.erase({src_side_id, dst_side_id});
  return SEDS_OK;
}

SedsResult seds_router_set_typed_route(SedsRouter * r, int32_t src_side_id, uint32_t ty, int32_t dst_side_id,
                                       bool enabled)
{
  if (r == nullptr || !seds::valid_type(ty))
    return SEDS_BAD_ARG;
  std::scoped_lock lock(r->mu);
  r->typed_route_overrides[{src_side_id, ty, dst_side_id}] = enabled;
  return SEDS_OK;
}

SedsResult seds_router_clear_typed_route(SedsRouter * r, int32_t src_side_id, uint32_t ty, int32_t dst_side_id)
{
  if (r == nullptr)
    return SEDS_BAD_ARG;
  std::scoped_lock lock(r->mu);
  r->typed_route_overrides.erase({src_side_id, ty, dst_side_id});
  return SEDS_OK;
}

SedsResult seds_router_set_source_route_mode(SedsRouter * r, int32_t src_side_id, SedsRouteSelectionMode mode)
{
  if (r == nullptr)
    return SEDS_BAD_ARG;
  std::scoped_lock lock(r->mu);
  policy_for(*r, src_side_id).mode = mode;
  return SEDS_OK;
}

SedsResult seds_router_clear_source_route_mode(SedsRouter * r, int32_t src_side_id)
{
  return seds_router_set_source_route_mode(r, src_side_id, Seds_RSM_Fanout);
}

SedsResult seds_router_set_route_weight(SedsRouter * r, int32_t src_side_id, int32_t dst_side_id, uint32_t weight)
{
  if (r == nullptr)
    return SEDS_BAD_ARG;
  std::scoped_lock lock(r->mu);
  policy_for(*r, src_side_id).weights[dst_side_id] = weight;
  return SEDS_OK;
}

SedsResult seds_router_clear_route_weight(SedsRouter * r, int32_t src_side_id, int32_t dst_side_id)
{
  if (r == nullptr)
    return SEDS_BAD_ARG;
  std::scoped_lock lock(r->mu);
  policy_for(*r, src_side_id).weights.erase(dst_side_id);
  return SEDS_OK;
}

SedsResult seds_router_set_route_priority(SedsRouter * r, int32_t src_side_id, int32_t dst_side_id, uint32_t priority)
{
  if (r == nullptr)
    return SEDS_BAD_ARG;
  std::scoped_lock lock(r->mu);
  policy_for(*r, src_side_id).priorities[dst_side_id] = priority;
  return SEDS_OK;
}

SedsResult seds_router_clear_route_priority(SedsRouter * r, int32_t src_side_id, int32_t dst_side_id)
{
  if (r == nullptr)
    return SEDS_BAD_ARG;
  std::scoped_lock lock(r->mu);
  policy_for(*r, src_side_id).priorities.erase(dst_side_id);
  return SEDS_OK;
}

int32_t seds_dtype_expected_size(SedsDataType ty)
{
  if (!seds::valid_type(ty))
    return SEDS_INVALID_TYPE;
  return seds::kTypeInfo[ty].dynamic
           ? -1
           : static_cast<int32_t>(seds::kTypeInfo[ty].elem_size * seds::kTypeInfo[ty].static_count);
}

SedsResult seds_router_log_bytes_ex(SedsRouter * r, SedsDataType ty, const uint8_t * data, size_t len,
                                    const uint64_t * timestamp_ms_opt, int queue)
{
  if (r == nullptr || !seds::valid_type(ty) || data == nullptr || !seds::validate_payload(ty, len))
    return SEDS_SIZE_MISMATCH;
  seds::PacketData pkt;
  pkt.ty = ty;
  pkt.sender = r->sender;
  pkt.endpoints = seds::kTypeInfo[ty].endpoints;
  pkt.timestamp = timestamp_ms_opt ? *timestamp_ms_opt : r->now_ms();
  pkt.payload.assign(data, data + len);
  std::scoped_lock lock(r->mu);
  if (queue)
  {
    if (!seds::enqueue_tx(r->tx_queue, r->tx_queue_bytes, {std::move(pkt), std::nullopt, std::nullopt, true}))
      return SEDS_PACKET_TOO_LARGE;
  }
  else
  {
    seds::dispatch_local_from_packet(pkt, r->locals);
    if (!seds::enqueue_tx(r->tx_queue, r->tx_queue_bytes, {std::move(pkt), std::nullopt, std::nullopt, false}))
      return SEDS_PACKET_TOO_LARGE;
    while (auto item = seds::pop_tx(r->tx_queue, r->tx_queue_bytes))
    {
      const auto rc = seds::transmit_item(*r, *item);
      if (rc != SEDS_OK)
      {
        return rc;
      }
    }
  }
  return SEDS_OK;
}

SedsResult seds_router_log_f32_ex(SedsRouter * r, SedsDataType ty, const float * vals, size_t n_vals,
                                  const uint64_t * timestamp_ms_opt, int queue)
{
  return seds_router_log_bytes_ex(r, ty, reinterpret_cast<const uint8_t *>(vals), n_vals * sizeof(float),
                                  timestamp_ms_opt, queue);
}

SedsResult seds_router_log_typed_ex(SedsRouter * r, SedsDataType ty, const void * data, size_t count, size_t elem_size,
                                    SedsElemKind, const uint64_t * timestamp_ms_opt, int queue)
{
  return seds_router_log_bytes_ex(r, ty, static_cast<const uint8_t *>(data), count * elem_size, timestamp_ms_opt,
                                  queue);
}

SedsResult seds_router_log_string_ex(SedsRouter * r, SedsDataType ty, const char * bytes, size_t len,
                                     const uint64_t * timestamp_ms_opt, int queue)
{
  return seds_router_log_bytes_ex(r, ty, reinterpret_cast<const uint8_t *>(bytes), len, timestamp_ms_opt, queue);
}

SedsResult seds_router_log_bytes(SedsRouter * r, SedsDataType ty, const uint8_t * data, size_t len)
{
  return seds_router_log_bytes_ex(r, ty, data, len, nullptr, 0);
}

SedsResult seds_router_log_f32(SedsRouter * r, SedsDataType ty, const float * vals, size_t n_vals)
{
  return seds_router_log_f32_ex(r, ty, vals, n_vals, nullptr, 0);
}

SedsResult seds_router_log_typed(SedsRouter * r, SedsDataType ty, const void * data, size_t count, size_t elem_size,
                                 SedsElemKind kind)
{
  return seds_router_log_typed_ex(r, ty, data, count, elem_size, kind, nullptr, 0);
}

SedsResult seds_router_log_queue_typed(SedsRouter * r, SedsDataType ty, const void * data, size_t count,
                                       size_t elem_size,
                                       SedsElemKind kind)
{
  return seds_router_log_typed_ex(r, ty, data, count, elem_size, kind, nullptr, 1);
}

SedsResult seds_router_receive_serialized(SedsRouter * r, const uint8_t * bytes, size_t len)
{
  const auto frame = seds::peek_frame_info(bytes, len, true);
  const auto pkt = seds::deserialize_packet(bytes, len);
  if (r == nullptr || !frame || !pkt)
    return SEDS_DESERIALIZE;
  std::scoped_lock lock(r->mu);
  if (!seds::is_discovery_control_type(frame->envelope.ty) &&
      frame->envelope.ty != SEDS_DT_TIME_SYNC_ANNOUNCE && frame->envelope.ty != SEDS_DT_TIME_SYNC_REQUEST &&
      frame->envelope.ty != SEDS_DT_TIME_SYNC_RESPONSE)
  {
    seds::dispatch_local_serialized_handlers(frame->envelope, bytes, len, r->locals);
  }
  seds::router_receive_impl(*r, *pkt, std::nullopt);
  return SEDS_OK;
}

SedsResult seds_router_receive(SedsRouter * r, const SedsPacketView * view)
{
  seds::PacketData pkt;
  if (r == nullptr || !seds::packet_from_view(view, pkt))
    return SEDS_BAD_ARG;
  std::scoped_lock lock(r->mu);
  seds::router_receive_impl(*r, std::move(pkt), std::nullopt);
  return SEDS_OK;
}

SedsResult seds_router_transmit_message(SedsRouter * r, const SedsPacketView * view)
{
  seds::PacketData pkt;
  if (r == nullptr || !seds::packet_from_view(view, pkt))
    return SEDS_BAD_ARG;
  if (all_endpoints_local(*r, pkt))
  {
    return seds::dispatch_local_packet_handlers(pkt, r->locals);
  }
  const auto rc = seds_router_transmit_message_queue(r, view);
  return rc == SEDS_OK ? seds_router_process_tx_queue(r) : rc;
}

SedsResult seds_router_transmit_message_queue(SedsRouter * r, const SedsPacketView * view)
{
  seds::PacketData pkt;
  if (r == nullptr || !seds::packet_from_view(view, pkt))
    return SEDS_BAD_ARG;
  std::scoped_lock lock(r->mu);
  if (all_endpoints_local(*r, pkt))
  {
    return seds::enqueue_rx(r->rx_queue, r->rx_queue_bytes, {std::move(pkt), std::nullopt, {}})
             ? SEDS_OK
             : SEDS_PACKET_TOO_LARGE;
  }
  return seds::enqueue_tx(r->tx_queue, r->tx_queue_bytes, {std::move(pkt), std::nullopt, std::nullopt, true})
           ? SEDS_OK
           : SEDS_PACKET_TOO_LARGE;
}

SedsResult seds_router_transmit_serialized_message_queue(SedsRouter * r, const uint8_t * bytes, size_t len)
{
  const auto pkt = seds::deserialize_packet(bytes, len);
  if (r == nullptr || !pkt)
    return SEDS_DESERIALIZE;
  std::scoped_lock lock(r->mu);
  return seds::enqueue_tx(r->tx_queue, r->tx_queue_bytes, {*pkt, std::nullopt, std::nullopt, true})
           ? SEDS_OK
           : SEDS_PACKET_TOO_LARGE;
}

SedsResult seds_router_transmit_serialized_message(SedsRouter * r, const uint8_t * bytes, size_t len)
{
  const auto rc = seds_router_transmit_serialized_message_queue(r, bytes, len);
  return rc == SEDS_OK ? seds_router_process_tx_queue(r) : rc;
}

SedsResult seds_router_process_tx_queue(SedsRouter * r) { return seds_router_process_tx_queue_with_timeout(r, 0); }
SedsResult seds_router_process_rx_queue(SedsRouter * r) { return seds_router_process_rx_queue_with_timeout(r, 0); }

SedsResult seds_router_process_tx_queue_with_timeout(SedsRouter * r, uint32_t)
{
  if (r == nullptr)
    return SEDS_BAD_ARG;
  std::scoped_lock lock(r->mu);
  seds::process_reliable_timeouts(*r);
  seds::process_end_to_end_reliable_timeouts(*r);
  while (auto item = seds::pop_tx(r->tx_queue, r->tx_queue_bytes))
  {
    const auto rc = seds::transmit_item(*r, *item);
    if (rc != SEDS_OK)
    {
      return rc;
    }
  }
  return SEDS_OK;
}

SedsResult seds_router_process_rx_queue_with_timeout(SedsRouter * r, uint32_t)
{
  if (r == nullptr)
    return SEDS_BAD_ARG;
  std::scoped_lock lock(r->mu);
  while (auto item = seds::pop_rx(r->rx_queue, r->rx_queue_bytes))
  {
    if (!item->wire_bytes.empty())
    {
      auto frame = seds::peek_frame_info(item->wire_bytes.data(), item->wire_bytes.size(), true);
      if (frame && !seds::is_discovery_control_type(frame->envelope.ty) &&
          frame->envelope.ty != SEDS_DT_RELIABLE_ACK && frame->envelope.ty != SEDS_DT_RELIABLE_PACKET_REQUEST &&
          frame->envelope.ty != SEDS_DT_TIME_SYNC_ANNOUNCE && frame->envelope.ty != SEDS_DT_TIME_SYNC_REQUEST &&
          frame->envelope.ty != SEDS_DT_TIME_SYNC_RESPONSE)
      {
        seds::dispatch_local_serialized_handlers(frame->envelope, item->wire_bytes.data(), item->wire_bytes.size(),
                                                 r->locals);
      }
    }
    seds::router_receive_impl(*r, std::move(item->pkt), item->src_side);
  }
  return SEDS_OK;
}

SedsResult seds_router_process_all_queues_with_timeout(SedsRouter * r, uint32_t timeout_ms)
{
  if (r == nullptr)
    return SEDS_BAD_ARG;
  std::scoped_lock lock(r->mu);
  if (timeout_ms == 0)
  {
    do
    {
      seds::process_reliable_timeouts(*r);
      seds::process_end_to_end_reliable_timeouts(*r);

      if (auto item = seds::pop_rx(r->rx_queue, r->rx_queue_bytes))
      {
        if (!item->wire_bytes.empty())
        {
          auto frame = seds::peek_frame_info(item->wire_bytes.data(), item->wire_bytes.size(), true);
          if (frame && !seds::is_discovery_control_type(frame->envelope.ty) &&
              frame->envelope.ty != SEDS_DT_RELIABLE_ACK && frame->envelope.ty != SEDS_DT_RELIABLE_PACKET_REQUEST &&
              frame->envelope.ty != SEDS_DT_TIME_SYNC_ANNOUNCE && frame->envelope.ty != SEDS_DT_TIME_SYNC_REQUEST &&
              frame->envelope.ty != SEDS_DT_TIME_SYNC_RESPONSE)
          {
            seds::dispatch_local_serialized_handlers(frame->envelope, item->wire_bytes.data(), item->wire_bytes.size(),
                                                     r->locals);
          }
        }
        seds::router_receive_impl(*r, std::move(item->pkt), item->src_side);
      }

      if (auto item = seds::pop_tx(r->tx_queue, r->tx_queue_bytes))
      {
        const auto rc = seds::transmit_item(*r, *item);
        if (rc != SEDS_OK)
        {
          return rc;
        }
      }
    } while (!r->rx_queue.empty() || !r->tx_queue.empty());
    return SEDS_OK;
  }

  const auto tx_budget_ms = static_cast<uint64_t>(timeout_ms / 2u);
  const auto rx_budget_ms = static_cast<uint64_t>(timeout_ms) - tx_budget_ms;

  const uint64_t tx_start_ms = r->now_ms();
  while (true)
  {
    seds::process_reliable_timeouts(*r);
    seds::process_end_to_end_reliable_timeouts(*r);
    auto item = seds::pop_tx(r->tx_queue, r->tx_queue_bytes);
    if (!item.has_value())
    {
      break;
    }
    const auto rc = seds::transmit_item(*r, *item);
    if (rc != SEDS_OK)
    {
      return rc;
    }
    if (timeout_expired(tx_start_ms, r->now_ms(), static_cast<uint32_t>(tx_budget_ms)))
    {
      break;
    }
  }

  const uint64_t rx_start_ms = r->now_ms();
  while (auto item = seds::pop_rx(r->rx_queue, r->rx_queue_bytes))
  {
    if (!item->wire_bytes.empty())
    {
      auto frame = seds::peek_frame_info(item->wire_bytes.data(), item->wire_bytes.size(), true);
      if (frame && !seds::is_discovery_control_type(frame->envelope.ty) &&
          frame->envelope.ty != SEDS_DT_RELIABLE_ACK && frame->envelope.ty != SEDS_DT_RELIABLE_PACKET_REQUEST &&
          frame->envelope.ty != SEDS_DT_TIME_SYNC_ANNOUNCE && frame->envelope.ty != SEDS_DT_TIME_SYNC_REQUEST &&
          frame->envelope.ty != SEDS_DT_TIME_SYNC_RESPONSE)
      {
        seds::dispatch_local_serialized_handlers(frame->envelope, item->wire_bytes.data(), item->wire_bytes.size(),
                                                 r->locals);
      }
    }
    seds::router_receive_impl(*r, std::move(item->pkt), item->src_side);
    if (timeout_expired(rx_start_ms, r->now_ms(), static_cast<uint32_t>(rx_budget_ms)))
    {
      break;
    }
  }

  return SEDS_OK;
}

SedsResult seds_router_process_all_queues(SedsRouter * r) { return seds_router_process_all_queues_with_timeout(r, 0); }

SedsResult seds_router_clear_queues(SedsRouter * r)
{
  if (!r)
    return SEDS_BAD_ARG;
  std::scoped_lock lock(r->mu);
  seds::clear_tx_queue(r->tx_queue, r->tx_queue_bytes);
  seds::clear_rx_queue(r->rx_queue, r->rx_queue_bytes);
  return SEDS_OK;
}

SedsResult seds_router_clear_rx_queue(SedsRouter * r)
{
  if (!r)
    return SEDS_BAD_ARG;
  std::scoped_lock lock(r->mu);
  seds::clear_rx_queue(r->rx_queue, r->rx_queue_bytes);
  return SEDS_OK;
}

SedsResult seds_router_clear_tx_queue(SedsRouter * r)
{
  if (!r)
    return SEDS_BAD_ARG;
  std::scoped_lock lock(r->mu);
  seds::clear_tx_queue(r->tx_queue, r->tx_queue_bytes);
  return SEDS_OK;
}

SedsResult seds_router_receive_serialized_from_side(SedsRouter * r, uint32_t side_id, const uint8_t * bytes, size_t len)
{
  const auto frame = seds::peek_frame_info(bytes, len, true);
  if (r == nullptr || !frame)
    return SEDS_DESERIALIZE;
  std::scoped_lock lock(r->mu);
  if (static_cast<size_t>(side_id) >= r->sides.size())
    return SEDS_INVALID_LINK_ID;
  if (!r->sides[side_id].ingress_enabled)
    return SEDS_INVALID_LINK_ID;
  const auto pkt = seds::deserialize_packet(bytes, len);
  if (!pkt)
    return SEDS_DESERIALIZE;
  if (!seds::process_reliable_ingress(*r, static_cast<int32_t>(side_id), *frame))
  {
    return SEDS_OK;
  }
  if (!seds::is_discovery_control_type(frame->envelope.ty) &&
      frame->envelope.ty != SEDS_DT_RELIABLE_ACK && frame->envelope.ty != SEDS_DT_RELIABLE_PACKET_REQUEST &&
      frame->envelope.ty != SEDS_DT_TIME_SYNC_ANNOUNCE && frame->envelope.ty != SEDS_DT_TIME_SYNC_REQUEST &&
      frame->envelope.ty != SEDS_DT_TIME_SYNC_RESPONSE)
  {
    seds::dispatch_local_serialized_handlers(frame->envelope, bytes, len, r->locals);
  }
  seds::router_receive_impl(*r, *pkt, static_cast<int32_t>(side_id));
  return SEDS_OK;
}

SedsResult seds_router_receive_from_side(SedsRouter * r, uint32_t side_id, const SedsPacketView * view)
{
  seds::PacketData pkt;
  if (r == nullptr || !seds::packet_from_view(view, pkt))
    return SEDS_BAD_ARG;
  std::scoped_lock lock(r->mu);
  if (static_cast<size_t>(side_id) >= r->sides.size())
    return SEDS_INVALID_LINK_ID;
  if (!r->sides[side_id].ingress_enabled)
    return SEDS_INVALID_LINK_ID;
  seds::router_receive_impl(*r, std::move(pkt), static_cast<int32_t>(side_id));
  return SEDS_OK;
}

SedsResult seds_router_rx_serialized_packet_to_queue(SedsRouter * r, const uint8_t * bytes, size_t len)
{
  auto pkt = seds::deserialize_packet(bytes, len);
  if (r == nullptr || !pkt)
    return SEDS_DESERIALIZE;
  std::scoped_lock lock(r->mu);
  return seds::enqueue_rx(r->rx_queue, r->rx_queue_bytes,
                          {std::move(*pkt), std::nullopt, std::vector<uint8_t>(bytes, bytes + len)})
           ? SEDS_OK
           : SEDS_PACKET_TOO_LARGE;
}

SedsResult seds_router_rx_packet_to_queue(SedsRouter * r, const SedsPacketView * view)
{
  seds::PacketData pkt;
  if (r == nullptr || !seds::packet_from_view(view, pkt))
    return SEDS_BAD_ARG;
  std::scoped_lock lock(r->mu);
  return seds::enqueue_rx(r->rx_queue, r->rx_queue_bytes, {std::move(pkt), std::nullopt, {}})
           ? SEDS_OK
           : SEDS_PACKET_TOO_LARGE;
}

SedsResult seds_router_rx_serialized_packet_to_queue_from_side(SedsRouter * r, uint32_t side_id, const uint8_t * bytes,
                                                               size_t len)
{
  const auto frame = seds::peek_frame_info(bytes, len, true);
  if (r == nullptr || !frame)
    return SEDS_DESERIALIZE;
  std::scoped_lock lock(r->mu);
  if (static_cast<size_t>(side_id) >= r->sides.size())
    return SEDS_INVALID_LINK_ID;
  if (!r->sides[side_id].ingress_enabled)
    return SEDS_INVALID_LINK_ID;
  const auto pkt = seds::deserialize_packet(bytes, len);
  if (!pkt)
    return SEDS_DESERIALIZE;
  if (!seds::process_reliable_ingress(*r, static_cast<int32_t>(side_id), *frame))
  {
    return SEDS_OK;
  }
  return seds::enqueue_rx(r->rx_queue, r->rx_queue_bytes,
                          {*pkt, static_cast<int32_t>(side_id), std::vector<uint8_t>(bytes, bytes + len)})
           ? SEDS_OK
           : SEDS_PACKET_TOO_LARGE;
}

SedsResult seds_router_rx_packet_to_queue_from_side(SedsRouter * r, uint32_t side_id, const SedsPacketView * view)
{
  seds::PacketData pkt;
  if (r == nullptr || !seds::packet_from_view(view, pkt))
    return SEDS_BAD_ARG;
  std::scoped_lock lock(r->mu);
  if (static_cast<size_t>(side_id) >= r->sides.size())
    return SEDS_INVALID_LINK_ID;
  if (!r->sides[side_id].ingress_enabled)
    return SEDS_INVALID_LINK_ID;
  return seds::enqueue_rx(r->rx_queue, r->rx_queue_bytes, {std::move(pkt), static_cast<int32_t>(side_id), {}})
           ? SEDS_OK
           : SEDS_PACKET_TOO_LARGE;
}
} // extern "C"
