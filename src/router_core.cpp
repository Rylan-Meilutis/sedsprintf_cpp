#include "internal.hpp"

#include <algorithm>
#include <ranges>

namespace seds
{
  namespace
  {
    constexpr std::string_view kEndToEndAckSender = "E2EACK";
    constexpr std::string_view kEndToEndAckPrefix = "E2EACK:";

    bool is_internal_control_type(const uint32_t ty)
    {
      return ty == SEDS_DT_RELIABLE_ACK || ty == SEDS_DT_RELIABLE_PARTIAL_ACK ||
             ty == SEDS_DT_RELIABLE_PACKET_REQUEST;
    }

    template<typename OwnerT>
    SedsResult call_serialized_tx(OwnerT & owner, Side & side, const std::vector<uint8_t> & bytes)
    {
      if (owner.side_tx_active)
      {
        owner.side_tx_deferred = true;
        return SEDS_IO;
      }
      owner.side_tx_active = true;
      const auto reset = std::unique_ptr<void, void (*)(void *)>(&owner, [](void * ptr)
      {
        static_cast<OwnerT *>(ptr)->side_tx_active = false;
      });
      return side.serialized_tx(bytes.data(), bytes.size(), side.user);
    }

    bool is_end_to_end_ack_sender(const std::string_view sender)
    {
      return sender == kEndToEndAckSender || sender.starts_with(kEndToEndAckPrefix);
    }

    std::optional<uint64_t> decode_end_to_end_ack_sender_hash(const std::string_view sender)
    {
      if (!sender.starts_with(kEndToEndAckPrefix) || sender.size() <= kEndToEndAckPrefix.size())
      {
        return std::nullopt;
      }
      return sender_hash(sender.substr(kEndToEndAckPrefix.size()));
    }

    std::string encode_end_to_end_ack_sender(const SedsRouter & r)
    {
      return std::string(kEndToEndAckPrefix) + r.sender;
    }

    bool is_end_to_end_destination_sender(const std::string_view sender)
    {
      return sender != "RELAY" && !is_end_to_end_ack_sender(sender);
    }

    bool board_matches_any_endpoint(const TopologyBoardNode & board, std::span<const uint32_t> endpoints)
    {
      return std::ranges::any_of(endpoints, [&](const uint32_t ep)
      {
        return std::ranges::find(board.reachable_endpoints, ep) != board.reachable_endpoints.end();
      });
    }

    std::unordered_map<uint64_t, int32_t> expected_end_to_end_destinations(const SedsRouter & r, const PacketData & pkt)
    {
      std::unordered_map<uint64_t, int32_t> out;
      const uint64_t now_ms = r.now_ms();
      const bool require_link_local = packet_requires_link_local(pkt);
      for (const auto & [side_id, route]: r.discovery_routes)
      {
        if (now_ms - route.last_seen_ms > kDiscoveryTtlMs)
        {
          continue;
        }
        if (side_id < 0 || static_cast<size_t>(side_id) >= r.sides.size())
        {
          continue;
        }
        const auto & side = r.sides[side_id];
        if (!side.egress_enabled || (require_link_local && !side.link_local_enabled))
        {
          continue;
        }
        for (const auto & [sender, sender_state]: route.announcers)
        {
          bool matched_board = false;
          for (const auto & board: sender_state.topology_boards)
          {
            if (!is_end_to_end_destination_sender(board.sender_id))
            {
              continue;
            }
            if (board_matches_any_endpoint(board, pkt.endpoints))
            {
              out[sender_hash(board.sender_id)] = side_id;
              matched_board = true;
            }
          }
          if (!matched_board && sender_state.topology_boards.empty() && is_end_to_end_destination_sender(sender))
          {
            for (const uint32_t ep: pkt.endpoints)
            {
              if (sender_state.endpoints.contains(ep))
              {
                out[sender_hash(sender)] = side_id;
                break;
              }
            }
          }
        }
      }
      return out;
    }

    std::vector<int32_t> filter_end_to_end_satisfied_sides(const SedsRelay & owner, const PacketData & pkt,
                                                           std::vector<int32_t> sides)
    {
      if (!is_reliable_type(pkt.ty) || is_internal_control_type(pkt.ty))
      {
        return sides;
      }
      const auto acked_it = owner.end_to_end_acked_destinations.find(packet_id(pkt));
      if (acked_it == owner.end_to_end_acked_destinations.end())
      {
        return sides;
      }
      const auto & acked = acked_it->second;
      std::vector<int32_t> filtered;
      for (const int32_t side: sides)
      {
        const auto route_it = owner.discovery_routes.find(side);
        if (route_it == owner.discovery_routes.end() || route_it->second.announcers.empty())
        {
          filtered.push_back(side);
          continue;
        }
        bool still_pending = false;
        bool had_destination_board = false;
        for (const auto & [sender, sender_state]: route_it->second.announcers)
        {
          for (const auto & board: sender_state.topology_boards)
          {
            if (!is_end_to_end_destination_sender(board.sender_id))
            {
              continue;
            }
            had_destination_board = true;
            if (board_matches_any_endpoint(board, pkt.endpoints) && !acked.contains(sender_hash(board.sender_id)))
            {
              still_pending = true;
              break;
            }
          }
          if (!had_destination_board && sender_state.topology_boards.empty() &&
              is_end_to_end_destination_sender(sender))
          {
            had_destination_board = true;
            for (const uint32_t ep: pkt.endpoints)
            {
              if (sender_state.endpoints.contains(ep) && !acked.contains(sender_hash(sender)))
              {
                still_pending = true;
                break;
              }
            }
          }
          if (still_pending)
          {
            break;
          }
        }
        if (still_pending || !had_destination_board)
        {
          filtered.push_back(side);
        }
      }
      return filtered;
    }

    std::string tx_error_message(const Side & side, int32_t rc)
    {
      return "TX handler failed on side " + side.name + ": " + error_string(rc);
    }

    std::string local_handler_error_message(uint32_t endpoint, int32_t rc)
    {
      const std::string ep_name =
          valid_endpoint(endpoint) ? std::string(kEndpointNames[endpoint]) : std::to_string(endpoint);
      return "Handler for endpoint " + ep_name + " failed: " + error_string(rc);
    }

    std::string_view control_sender(const SedsRouter & r) { return r.sender; }

    std::string_view control_sender(const SedsRelay &) { return "RELAY"; }

    template<typename OwnerT>
    void handle_ack_impl(OwnerT & owner, int32_t side_id, uint32_t ty, uint32_t ack)
    {
      auto key = reliable_key(side_id, ty);
      auto it = owner.reliable_tx.find(key);
      if (it == owner.reliable_tx.end())
      {
        return;
      }
      auto & tx = it->second;
      if (kTypeInfo[ty].reliable_mode == ReliableMode::Unordered)
      {
        tx.sent.erase(ack);
        std::erase(tx.sent_order, ack);
        return;
      }
      while (!tx.sent_order.empty() && tx.sent_order.front() <= ack)
      {
        tx.sent.erase(tx.sent_order.front());
        tx.sent_order.pop_front();
      }
    }

    template<typename OwnerT>
    void handle_partial_ack_impl(OwnerT & owner, int32_t side_id, uint32_t ty, uint32_t seq)
    {
      auto it = owner.reliable_tx.find(reliable_key(side_id, ty));
      if (it == owner.reliable_tx.end())
      {
        return;
      }
      if (auto sent = it->second.sent.find(seq); sent != it->second.sent.end())
      {
        sent->second.partial_acked = true;
      }
    }

    template<typename OwnerT>
    void handle_packet_request_impl(OwnerT & owner, const int32_t side_id, const uint32_t ty,
                                    const uint32_t seq)
    {
      if (side_id < 0 || static_cast<size_t>(side_id) >= owner.sides.size())
      {
        return;
      }
      const auto key = reliable_key(side_id, ty);
      const auto it = owner.reliable_tx.find(key);
      if (it == owner.reliable_tx.end())
      {
        return;
      }
      const auto sent = it->second.sent.find(seq);
      if (sent == it->second.sent.end())
      {
        return;
      }
      auto & side = owner.sides[side_id];
      if (side.serialized_tx != nullptr)
      {
        static_cast<void>(call_serialized_tx(owner, side, sent->second.bytes));
        sent->second.last_send_ms = owner.now_ms();
        sent->second.retries++;
        sent->second.queued = false;
        sent->second.partial_acked = false;
      }
    }

    template<typename OwnerT>
    void queue_reliable_ack(OwnerT & owner, const int32_t side_id, const uint32_t ty,
                            const uint32_t seq)
    {
      if (side_id < 0 || static_cast<size_t>(side_id) >= owner.sides.size())
      {
        return;
      }
      const auto & side = owner.sides[side_id];
      if (!side.reliable_enabled || side.serialized_tx == nullptr)
      {
        return;
      }
      enqueue_tx_front(owner.tx_queue, owner.tx_queue_bytes,
                       {
                         make_reliable_control_packet(SEDS_DT_RELIABLE_ACK, ty, seq, owner.now_ms(),
                                                      control_sender(owner)),
                         std::nullopt,
                         side_id, false
                       });
    }

    template<typename OwnerT>
    void queue_reliable_packet_request(OwnerT & owner, const int32_t side_id, const uint32_t ty,
                                       const uint32_t seq)
    {
      if (side_id < 0 || static_cast<size_t>(side_id) >= owner.sides.size())
      {
        return;
      }
      const auto & side = owner.sides[side_id];
      if (!side.reliable_enabled || side.serialized_tx == nullptr)
      {
        return;
      }
      enqueue_tx_front(owner.tx_queue, owner.tx_queue_bytes,
                       {
                         make_reliable_control_packet(SEDS_DT_RELIABLE_PACKET_REQUEST, ty, seq, owner.now_ms(),
                                                      control_sender(owner)),
                         std::nullopt, side_id, false
                       });
    }

    template<typename OwnerT>
    void queue_reliable_partial_ack(OwnerT & owner, const int32_t side_id, const uint32_t ty, const uint32_t seq)
    {
      if (side_id < 0 || static_cast<size_t>(side_id) >= owner.sides.size())
      {
        return;
      }
      const auto & side = owner.sides[side_id];
      if (!side.reliable_enabled || side.serialized_tx == nullptr)
      {
        return;
      }
      enqueue_tx_front(owner.tx_queue, owner.tx_queue_bytes,
                       {
                         make_reliable_control_packet(SEDS_DT_RELIABLE_PARTIAL_ACK, ty, seq, owner.now_ms(),
                                                      control_sender(owner)),
                         std::nullopt, side_id, false
                       });
    }

    void queue_end_to_end_ack(SedsRouter & r, const PacketData & pkt, std::optional<int32_t> src_side)
    {
      const auto sender = encode_end_to_end_ack_sender(r);
      enqueue_tx_front(r.tx_queue, r.tx_queue_bytes,
                       {
                         make_e2e_reliable_ack_packet(packet_id(pkt), r.now_ms(), sender),
                         std::nullopt, src_side, false
                       });
      const uint64_t source_id = source_packet_id(pkt);
      // Rust source routers may track multi-endpoint packets in schema endpoint order, while
      // relays route ACKs by the wire-decoded endpoint order. Emit both IDs when they differ.
      if (source_id != packet_id(pkt))
      {
        enqueue_tx_front(r.tx_queue, r.tx_queue_bytes,
                         {
                           make_e2e_reliable_ack_packet(source_id, r.now_ms(), sender),
                           std::nullopt, src_side, false
                         });
      }
    }

    template<typename OwnerT>
    bool process_reliable_ingress_impl(OwnerT & owner, int32_t side_id, const FrameInfoLite & frame,
                                       const PacketData & pkt, std::span<const uint8_t> wire_bytes)
    {
      if (!frame.reliable.has_value())
      {
        return true;
      }

      const auto & hdr = *frame.reliable;

      if ((hdr.flags & kReliableFlagUnsequenced) != 0u)
      {
        return true;
      }

      auto & rx = owner.reliable_rx[reliable_key(side_id, frame.envelope.ty)];
      if ((hdr.flags & kReliableFlagUnordered) != 0u)
      {
        rx.last_ack = hdr.seq;
        queue_reliable_ack(owner, side_id, frame.envelope.ty, hdr.seq);
        return true;
      }

      if (hdr.seq < rx.expected_seq)
      {
        rx.last_ack = rx.expected_seq > 0 ? rx.expected_seq - 1 : 0;
        queue_reliable_ack(owner, side_id, frame.envelope.ty, rx.last_ack);
        return false;
      }

      if (hdr.seq > rx.expected_seq)
      {
        if (rx.buffered.size() < kReliableMaxPending)
        {
          rx.buffered.emplace(hdr.seq, ReliableRxState::Buffered{pkt, {wire_bytes.begin(), wire_bytes.end()}});
        }
        queue_reliable_partial_ack(owner, side_id, frame.envelope.ty, hdr.seq);
        queue_reliable_packet_request(owner, side_id, frame.envelope.ty, rx.expected_seq);
        return false;
      }

      const uint32_t ack = hdr.seq;
      uint32_t next = rx.expected_seq + 1;
      if (next == 0)
      {
        next = 1;
      }
      rx.expected_seq = next;
      rx.last_ack = ack;
      while (true)
      {
        auto buffered = rx.buffered.extract(rx.expected_seq);
        if (buffered.empty())
        {
          break;
        }
        owner.reliable_released_rx.push_back(
          {std::move(buffered.mapped().pkt), side_id, std::move(buffered.mapped().wire_bytes)});
        rx.last_ack = rx.expected_seq;
        const uint32_t after = rx.expected_seq + 1;
        rx.expected_seq = after == 0 ? 1 : after;
      }
      queue_reliable_ack(owner, side_id, frame.envelope.ty, rx.last_ack);
      return true;
    }

    template<typename OwnerT>
    SedsResult send_serialized_with_reliable(OwnerT & owner, int32_t side_id, const PacketData & pkt)
    {
      auto & side = owner.sides[side_id];
      if (side.serialized_tx == nullptr)
      {
        return SEDS_OK;
      }
      if (!side.reliable_enabled || !is_reliable_type(pkt.ty))
      {
        auto bytes = serialize_packet(pkt);
        return call_serialized_tx(owner, side, bytes);
      }
      auto key = reliable_key(side_id, pkt.ty);
      auto & tx = owner.reliable_tx[key];
      if (tx.sent.size() >= kReliableMaxPending)
      {
        return SEDS_PACKET_TOO_LARGE;
      }
      const uint8_t flags = kTypeInfo[pkt.ty].reliable_mode == ReliableMode::Unordered ? kReliableFlagUnordered : 0u;
      auto bytes = serialize_packet_with_reliable(pkt, ReliableHeaderLite{flags, tx.next_seq, 0u});
      const uint32_t seq = tx.next_seq++;
      if (tx.next_seq == 0)
      {
        tx.next_seq = 1;
      }
      const auto rc = call_serialized_tx(owner, side, bytes);
      if (rc != SEDS_OK)
      {
        return rc;
      }
      tx.sent_order.push_back(seq);
      tx.sent.emplace(seq, ReliableTxState::Sent{std::move(bytes), owner.now_ms(), 0, false, false});
      return SEDS_OK;
    }

    template<typename OwnerT>
    void do_reliable_timeouts(OwnerT & owner)
    {
      const uint64_t now = owner.now_ms();
      for (auto & [key, tx]: owner.reliable_tx)
      {
        const auto side_id = static_cast<int32_t>(key >> 32u);
        if (side_id < 0 || static_cast<size_t>(side_id) >= owner.sides.size())
        {
          continue;
        }
        auto & side = owner.sides[side_id];
        if (side.serialized_tx == nullptr)
        {
          continue;
        }
        const auto seqs = std::vector<uint32_t>(tx.sent_order.begin(), tx.sent_order.end());
        for (const uint32_t seq: seqs)
        {
          auto sent = tx.sent.find(seq);
          if (sent == tx.sent.end() || sent->second.queued || sent->second.partial_acked ||
              now - sent->second.last_send_ms < kReliableRetransmitMs)
          {
            continue;
          }
          if (sent->second.retries >= kReliableMaxRetries)
          {
            tx.sent.erase(sent);
            std::erase(tx.sent_order, seq);
            continue;
          }
          const auto rc = call_serialized_tx(owner, side, sent->second.bytes);
          if (rc == SEDS_OK)
          {
            sent->second.last_send_ms = now;
            sent->second.retries++;
          }
        }
      }
    }
  } // namespace

  void push_recent(SedsRouter & r, const uint64_t id)
  {
    if (r.recent_set.contains(id))
    {
      return;
    }
    constexpr size_t kRecentCap = 128;
    if (r.recent_ids.size() >= kRecentCap)
    {
      r.recent_set.erase(r.recent_ids.front());
      r.recent_ids.pop_front();
    }
    r.recent_ids.push_back(id);
    r.recent_set.insert(id);
  }

  void push_recent(SedsRelay & r, const uint64_t id)
  {
    if (r.recent_set.contains(id))
    {
      return;
    }
    constexpr size_t kRecentCap = 128;
    if (r.recent_ids.size() >= kRecentCap)
    {
      r.recent_set.erase(r.recent_ids.front());
      r.recent_ids.pop_front();
    }
    r.recent_ids.push_back(id);
    r.recent_set.insert(id);
  }

  void note_reliable_return_route(SedsRouter & r, const int32_t side_id, const uint64_t packet_id)
  {
    r.reliable_return_routes[packet_id] = ReliableReturnRouteState{side_id};
  }

  void note_reliable_return_route(SedsRelay & r, const int32_t side_id, const uint64_t packet_id)
  {
    r.reliable_return_routes[packet_id] = ReliableReturnRouteState{side_id};
  }

  void reconcile_end_to_end_reliable_destinations(SedsRouter & r)
  {
    for (auto it = r.end_to_end_reliable_tx.begin(); it != r.end_to_end_reliable_tx.end();)
    {
      auto & sent = it->second;
      if (!sent.tracked_destinations)
      {
        ++it;
        continue;
      }
      const bool require_link_local = packet_requires_link_local(sent.pkt);
      for (auto pending_it = sent.pending_destinations.begin(); pending_it != sent.pending_destinations.end();)
      {
        const int32_t side = pending_it->second;
        if (side < 0 || static_cast<size_t>(side) >= r.sides.size() || !r.sides[side].egress_enabled ||
            (require_link_local && !r.sides[side].link_local_enabled))
        {
          pending_it = sent.pending_destinations.erase(pending_it);
          continue;
        }
        const auto route_it = r.discovery_routes.find(side);
        bool found = false;
        if (route_it != r.discovery_routes.end() && r.now_ms() - route_it->second.last_seen_ms <= kDiscoveryTtlMs)
        {
          for (const auto & [sender, sender_state]: route_it->second.announcers)
          {
            for (const auto & board: sender_state.topology_boards)
            {
              if (!is_end_to_end_destination_sender(board.sender_id))
              {
                continue;
              }
              if (sender_hash(board.sender_id) == pending_it->first)
              {
                found = true;
                break;
              }
            }
            if (!found && sender_state.topology_boards.empty() && is_end_to_end_destination_sender(sender) &&
                sender_hash(sender) == pending_it->first)
            {
              found = true;
            }
            if (found)
            {
              break;
            }
          }
        }
        if (!found)
        {
          pending_it = sent.pending_destinations.erase(pending_it);
        }
        else
        {
          ++pending_it;
        }
      }
      if (sent.pending_destinations.empty())
      {
        it = r.end_to_end_reliable_tx.erase(it);
      }
      else
      {
        ++it;
      }
    }
  }

  SedsResult dispatch_local_packet_handlers(const PacketData & pkt, const std::vector<LocalEndpoint> & locals)
  {
    SedsPacketView view{};
    fill_view(pkt, view);
    bool had_failure = false;
    std::vector<uint32_t> error_endpoints;
    uint32_t failing_endpoint = 0;
    int32_t failing_rc = SEDS_OK;
    for (const auto & local: locals)
    {
      if (std::ranges::find(pkt.endpoints, local.endpoint) == pkt.endpoints.end())
      {
        continue;
      }
      if (local.packet_handler != nullptr)
      {
        int32_t rc = SEDS_OK;
        for (size_t attempt = 0; attempt < 3; ++attempt)
        {
          rc = local.packet_handler(&view, local.user);
          if (rc == SEDS_OK)
          {
            break;
          }
        }
        if (rc != SEDS_OK)
        {
          had_failure = true;
          if (failing_rc == SEDS_OK)
          {
            failing_endpoint = local.endpoint;
            failing_rc = rc;
          }
          for (const auto & other: locals)
          {
            if (other.endpoint == local.endpoint || other.packet_handler == nullptr)
            {
              continue;
            }
            if (std::ranges::find(error_endpoints, other.endpoint) == error_endpoints.end())
            {
              error_endpoints.push_back(other.endpoint);
            }
          }
        }
      }
    }
    if (had_failure && pkt.ty != SEDS_DT_TELEMETRY_ERROR && !error_endpoints.empty())
    {
      const auto error_pkt =
          make_router_error_packet(std::move(error_endpoints),
                                   local_handler_error_message(failing_endpoint, failing_rc));
      SedsPacketView error_view{};
      fill_view(error_pkt, error_view);
      for (const auto & local: locals)
      {
        if (std::ranges::find(error_pkt.endpoints, local.endpoint) == error_pkt.endpoints.end())
        {
          continue;
        }
        if (local.packet_handler != nullptr)
        {
          local.packet_handler(&error_view, local.user);
        }
      }
    }
    return had_failure ? SEDS_HANDLER_ERROR : SEDS_OK;
  }

  void dispatch_local_serialized_handlers(const TelemetryEnvelopeLite & env, const uint8_t * bytes, size_t len,
                                          const std::vector<LocalEndpoint> & locals)
  {
    for (const auto & local: locals)
    {
      if (std::find(env.endpoints.begin(), env.endpoints.end(), local.endpoint) == env.endpoints.end())
      {
        continue;
      }
      if (local.serialized_handler != nullptr)
      {
        local.serialized_handler(bytes, len, local.user);
      }
    }
  }

  void dispatch_local_from_packet(const PacketData & pkt, const std::vector<LocalEndpoint> & locals)
  {
    static_cast<void>(dispatch_local_packet_handlers(pkt, locals));
    const auto bytes = serialize_packet(pkt);
    const TelemetryEnvelopeLite env{pkt.ty, pkt.endpoints, pkt.sender, pkt.timestamp};
    dispatch_local_serialized_handlers(env, bytes.data(), bytes.size(), locals);
  }

  static std::vector<int32_t> default_candidates(const std::vector<Side> & sides,
                                                 const std::unordered_map<int32_t, DiscoveryRoute> & routes,
                                                 const PacketData & pkt, std::optional<int32_t> src_side,
                                                 std::optional<std::string_view> preferred_timesync_source,
                                                 uint64_t now_ms)
  {
    std::vector<int32_t> out;
    const bool require_link_local = packet_requires_link_local(pkt);
    if (preferred_timesync_source.has_value())
    {
      const std::string preferred(*preferred_timesync_source);
      for (const auto & [side_id, route]: routes)
      {
        if (src_side && side_id == *src_side)
        {
          continue;
        }
        if (now_ms - route.last_seen_ms > kDiscoveryTtlMs)
        {
          continue;
        }
        if (side_id < 0 || static_cast<size_t>(side_id) >= sides.size())
        {
          continue;
        }
        if (require_link_local && !sides[side_id].link_local_enabled)
        {
          continue;
        }
        if (route.timesync_sources.contains(preferred))
        {
          out.push_back(side_id);
        }
      }
      if (!out.empty())
      {
        std::ranges::sort(out);
        out.erase(std::ranges::unique(out).begin(), out.end());
        return out;
      }
    }
    for (const auto & [side_id, route]: routes)
    {
      if (src_side && side_id == *src_side)
      {
        continue;
      }
      if (now_ms - route.last_seen_ms > kDiscoveryTtlMs)
      {
        continue;
      }
      if (side_id < 0 || static_cast<size_t>(side_id) >= sides.size())
      {
        continue;
      }
      if (require_link_local && !sides[side_id].link_local_enabled)
      {
        continue;
      }
      for (uint32_t ep: pkt.endpoints)
      {
        if (ep == SEDS_EP_DISCOVERY || route.endpoints.count(ep) != 0u)
        {
          out.push_back(side_id);
          break;
        }
      }
    }
    if (out.empty())
    {
      for (size_t i = 0; i < sides.size(); ++i)
      {
        if (src_side && static_cast<int32_t>(i) == *src_side)
        {
          continue;
        }
        if (sides[i].egress_enabled && (!require_link_local || sides[i].link_local_enabled))
        {
          out.push_back(static_cast<int32_t>(i));
        }
      }
    }
    std::ranges::sort(out);
    out.erase(std::ranges::unique(out).begin(), out.end());
    return out;
  }

  std::vector<int32_t>
  select_destinations(const std::vector<Side> & sides, const std::unordered_map<int32_t, DiscoveryRoute> & routes,
                      const std::unordered_map<RouteKey, bool, RouteKeyHash> & route_overrides,
                      const std::unordered_map<TypedRouteKey, bool, TypedRouteKeyHash> & typed_route_overrides,
                      RoutePolicy & policy, const PacketData & pkt, std::optional<int32_t> src_side,
                      std::optional<int32_t> explicit_dst, std::optional<std::string_view> preferred_timesync_source,
                      uint64_t now_ms)
  {
    if (explicit_dst)
    {
      return {*explicit_dst};
    }

    const auto candidates = default_candidates(sides, routes, pkt, src_side, preferred_timesync_source, now_ms);
    std::vector<int32_t> filtered;
    const int32_t src = src_side.value_or(-1);
    bool typed_override_present = false;
    for (const auto & [key, _]: typed_route_overrides)
    {
      if (key.src_side == src && key.ty == pkt.ty)
      {
        typed_override_present = true;
        break;
      }
    }
    for (const int32_t side_id: candidates)
    {
      if (side_id < 0 || static_cast<size_t>(side_id) >= sides.size())
      {
        continue;
      }
      if (!sides[side_id].egress_enabled)
      {
        continue;
      }
      if (!side_accepts_packet(sides[side_id], pkt))
      {
        continue;
      }
      const TypedRouteKey typed_key{src, pkt.ty, side_id};
      const RouteKey route_key{src, side_id};
      if (const auto it = route_overrides.find(route_key); it != route_overrides.end() && !it->second)
      {
        continue;
      }
      if (const auto it = typed_route_overrides.find(typed_key); it != typed_route_overrides.end())
      {
        if (it->second)
        {
          filtered.push_back(side_id);
        }
        continue;
      }
      if (typed_override_present)
      {
        continue;
      }
      if (const auto it = route_overrides.find(route_key); it != route_overrides.end())
      {
        if (it->second)
        {
          filtered.push_back(side_id);
        }
        continue;
      }
      filtered.push_back(side_id);
    }
    return apply_policy(policy, std::move(filtered));
  }

  SedsResult transmit_item(SedsRouter & owner, const TxItem & item)
  {
    if (item.deliver_local)
    {
      if (dispatch_local_packet_handlers(item.pkt, owner.locals) != SEDS_OK)
      {
        return SEDS_HANDLER_ERROR;
      }
      const auto bytes = serialize_packet(item.pkt);
      const TelemetryEnvelopeLite env{item.pkt.ty, item.pkt.endpoints, item.pkt.sender, item.pkt.timestamp};
      dispatch_local_serialized_handlers(env, bytes.data(), bytes.size(), owner.locals);
    }
    auto & policy = item.src_side ? owner.source_policy[*item.src_side] : owner.local_policy;
    const std::optional<std::string_view> preferred_timesync_source =
        item.pkt.ty == SEDS_DT_TIME_SYNC_REQUEST && !owner.timesync.current_source.empty()
          ? std::optional<std::string_view>(owner.timesync.current_source)
          : std::nullopt;
    const auto targets =
        select_destinations(owner.sides, owner.discovery_routes, owner.route_overrides, owner.typed_route_overrides,
                            policy, item.pkt, item.src_side, item.dst_side, preferred_timesync_source, owner.now_ms());
    if (is_reliable_type(item.pkt.ty) && !is_internal_control_type(item.pkt.ty))
    {
      for (const int32_t side_id: targets)
      {
        if (side_id < 0 || static_cast<size_t>(side_id) >= owner.sides.size())
        {
          continue;
        }
        const auto & side = owner.sides[side_id];
        if (side.reliable_enabled && side.serialized_tx != nullptr)
        {
          const auto pending = expected_end_to_end_destinations(owner, item.pkt);
          if (!pending.empty())
          {
            owner.end_to_end_reliable_tx[packet_id(item.pkt)] =
                EndToEndReliableSent{item.pkt, pending, true, owner.now_ms(), 0, false};
          }
          break;
        }
      }
    }
    SedsPacketView view{};
    fill_view(item.pkt, view);
    bool had_failure = false;
    for (int32_t side_id: targets)
    {
      if (side_id < 0 || static_cast<size_t>(side_id) >= owner.sides.size())
      {
        continue;
      }
      const auto & side = owner.sides[side_id];
      if (!side.egress_enabled)
      {
        continue;
      }
      if (side.serialized_tx != nullptr)
      {
        const auto rc = send_serialized_with_reliable(owner, side_id, item.pkt);
        if (rc != SEDS_OK)
        {
          if (rc == SEDS_IO && owner.side_tx_deferred)
          {
            owner.side_tx_deferred = false;
            return SEDS_IO;
          }
          had_failure = true;
          if (item.pkt.ty != SEDS_DT_TELEMETRY_ERROR)
          {
            auto err_pkt = make_router_error_packet(owner.locals, tx_error_message(side, rc));
            dispatch_local_from_packet(err_pkt, owner.locals);
          }
        }
      }
      else if (side.packet_tx != nullptr)
      {
        const auto rc = side.packet_tx(&view, side.user);
        if (rc != SEDS_OK)
        {
          had_failure = true;
          if (item.pkt.ty != SEDS_DT_TELEMETRY_ERROR)
          {
            auto err_pkt = make_router_error_packet(owner.locals, tx_error_message(side, rc));
            dispatch_local_from_packet(err_pkt, owner.locals);
          }
        }
      }
    }
    return had_failure ? SEDS_HANDLER_ERROR : SEDS_OK;
  }

  SedsResult transmit_item(SedsRelay & owner, const TxItem & item)
  {
    auto & policy = item.src_side ? owner.source_policy[*item.src_side] : owner.local_policy;
    const auto targets =
        select_destinations(owner.sides, owner.discovery_routes, owner.route_overrides, owner.typed_route_overrides,
                            policy, item.pkt, item.src_side, item.dst_side, std::nullopt, owner.now_ms());
    const auto filtered_targets = filter_end_to_end_satisfied_sides(owner, item.pkt, targets);
    SedsPacketView view{};
    fill_view(item.pkt, view);
    for (const int32_t side_id: filtered_targets)
    {
      if (side_id < 0 || static_cast<size_t>(side_id) >= owner.sides.size())
      {
        continue;
      }
      const auto & side = owner.sides[side_id];
      if (!side.egress_enabled)
      {
        continue;
      }
      if (side.serialized_tx != nullptr)
      {
        const auto rc = send_serialized_with_reliable(owner, side_id, item.pkt);
        if (rc != SEDS_OK)
        {
          if (rc == SEDS_IO && owner.side_tx_deferred)
          {
            owner.side_tx_deferred = false;
            return SEDS_IO;
          }
          return SEDS_HANDLER_ERROR;
        }
      }
      else if (side.packet_tx != nullptr)
      {
        const auto rc = side.packet_tx(&view, side.user);
        if (rc != SEDS_OK)
        {
          return SEDS_HANDLER_ERROR;
        }
      }
    }
    return SEDS_OK;
  }

  void router_receive_impl(SedsRouter & r, PacketData pkt, std::optional<int32_t> src_side)
  {
    const uint64_t id = packet_id(pkt);
    if (r.recent_set.count(id) != 0u)
    {
      if (src_side && is_reliable_type(pkt.ty) && !is_internal_control_type(pkt.ty))
      {
        const bool has_local_handler =
            std::ranges::any_of(r.locals, [&](const LocalEndpoint & local)
            {
              return std::ranges::find(pkt.endpoints, local.endpoint) != pkt.endpoints.end() &&
                     (local.packet_handler != nullptr || local.serialized_handler != nullptr);
            });
        if (has_local_handler)
        {
          queue_end_to_end_ack(r, pkt, src_side);
        }
      }
      return;
    }
    push_recent(r, id);
    if (src_side && is_reliable_type(pkt.ty) && !is_internal_control_type(pkt.ty))
    {
      note_reliable_return_route(r, *src_side, id);
      const uint64_t source_id = source_packet_id(pkt);
      if (source_id != id)
      {
        note_reliable_return_route(r, *src_side, source_id);
      }
    }
    if (pkt.ty == SEDS_DT_RELIABLE_ACK || pkt.ty == SEDS_DT_RELIABLE_PARTIAL_ACK ||
        pkt.ty == SEDS_DT_RELIABLE_PACKET_REQUEST)
    {
      if (pkt.ty == SEDS_DT_RELIABLE_ACK && is_end_to_end_ack_sender(pkt.sender) &&
          pkt.payload.size() == sizeof(uint64_t))
      {
        uint64_t acked_id = 0;
        std::memcpy(&acked_id, pkt.payload.data(), sizeof(acked_id));
        if (auto it = r.end_to_end_reliable_tx.find(acked_id); it != r.end_to_end_reliable_tx.end())
        {
          if (const auto ack_sender = decode_end_to_end_ack_sender_hash(pkt.sender); ack_sender.has_value())
          {
            it->second.pending_destinations.erase(*ack_sender);
            if (it->second.pending_destinations.empty())
            {
              r.end_to_end_reliable_tx.erase(it);
            }
          }
          else
          {
            r.end_to_end_reliable_tx.erase(it);
          }
        }
        return;
      }
      if (src_side && pkt.payload.size() == sizeof(uint32_t) * 2u)
      {
        uint32_t ty = 0;
        uint32_t seq = 0;
        std::memcpy(&ty, pkt.payload.data(), sizeof(ty));
        std::memcpy(&seq, pkt.payload.data() + sizeof(ty), sizeof(seq));
        if (pkt.ty == SEDS_DT_RELIABLE_ACK)
        {
          handle_ack_impl(r, *src_side, ty, seq);
        }
        else if (pkt.ty == SEDS_DT_RELIABLE_PARTIAL_ACK)
        {
          handle_partial_ack_impl(r, *src_side, ty, seq);
        }
        else
        {
          handle_packet_request_impl(r, *src_side, ty, seq);
        }
      }
      return;
    }
    if (is_discovery_control_type(pkt.ty))
    {
      handle_discovery_packet(r, pkt, src_side);
      return;
    }
    if (pkt.ty == SEDS_DT_TIME_SYNC_ANNOUNCE || pkt.ty == SEDS_DT_TIME_SYNC_REQUEST ||
        pkt.ty == SEDS_DT_TIME_SYNC_RESPONSE)
    {
      handle_timesync_packet(r, pkt, src_side);
      return;
    }
    const bool had_local_handler =
        std::ranges::any_of(r.locals, [&](const LocalEndpoint & local)
        {
          return std::ranges::find(pkt.endpoints, local.endpoint) != pkt.endpoints.end() &&
                 (local.packet_handler != nullptr || local.serialized_handler != nullptr);
        });
    static_cast<void>(dispatch_local_packet_handlers(pkt, r.locals));
    if (src_side && had_local_handler && is_reliable_type(pkt.ty))
    {
      queue_end_to_end_ack(r, pkt, src_side);
    }
    if (src_side && r.mode == Seds_RM_Relay)
    {
      enqueue_tx(r.tx_queue, r.tx_queue_bytes, {std::move(pkt), src_side, std::nullopt, false});
    }
  }

  void relay_receive_impl(SedsRelay & relay, PacketData pkt, std::optional<int32_t> src_side)
  {
    const uint64_t id = packet_id(pkt);
    if (src_side && is_reliable_type(pkt.ty) && !is_internal_control_type(pkt.ty))
    {
      note_reliable_return_route(relay, *src_side, id);
      const uint64_t source_id = source_packet_id(pkt);
      if (source_id != id)
      {
        note_reliable_return_route(relay, *src_side, source_id);
      }
    }
    if (pkt.ty == SEDS_DT_RELIABLE_ACK || pkt.ty == SEDS_DT_RELIABLE_PARTIAL_ACK ||
        pkt.ty == SEDS_DT_RELIABLE_PACKET_REQUEST)
    {
      if (pkt.ty == SEDS_DT_RELIABLE_ACK && is_end_to_end_ack_sender(pkt.sender) &&
          pkt.payload.size() == sizeof(uint64_t))
      {
        uint64_t acked_id = 0;
        std::memcpy(&acked_id, pkt.payload.data(), sizeof(acked_id));
        if (const auto ack_sender = decode_end_to_end_ack_sender_hash(pkt.sender); ack_sender.has_value())
        {
          relay.end_to_end_acked_destinations[acked_id].insert(*ack_sender);
        }
        if (const auto it = relay.reliable_return_routes.find(acked_id);
          src_side && it != relay.reliable_return_routes.end())
        {
          enqueue_tx(relay.tx_queue, relay.tx_queue_bytes, {std::move(pkt), src_side, it->second.side, false});
        }
        return;
      }
      if (src_side && pkt.payload.size() == sizeof(uint32_t) * 2u)
      {
        uint32_t ty = 0;
        uint32_t seq = 0;
        std::memcpy(&ty, pkt.payload.data(), sizeof(ty));
        std::memcpy(&seq, pkt.payload.data() + sizeof(ty), sizeof(seq));
        if (pkt.ty == SEDS_DT_RELIABLE_ACK)
        {
          handle_ack_impl(relay, *src_side, ty, seq);
        }
        else if (pkt.ty == SEDS_DT_RELIABLE_PARTIAL_ACK)
        {
          handle_partial_ack_impl(relay, *src_side, ty, seq);
        }
        else
        {
          handle_packet_request_impl(relay, *src_side, ty, seq);
        }
      }
      return;
    }
    if (is_discovery_control_type(pkt.ty))
    {
      handle_discovery_packet(relay, pkt, src_side);
    }
    enqueue_tx(relay.tx_queue, relay.tx_queue_bytes, {std::move(pkt), src_side, std::nullopt, false});
  }

  void process_reliable_timeouts(SedsRouter & r) { do_reliable_timeouts(r); }
  void process_reliable_timeouts(SedsRelay & r) { do_reliable_timeouts(r); }

  void process_end_to_end_reliable_timeouts(SedsRouter & r)
  {
    const uint64_t now = r.now_ms();
    std::vector<uint64_t> requeue;
    reconcile_end_to_end_reliable_destinations(r);
    for (auto it = r.end_to_end_reliable_tx.begin(); it != r.end_to_end_reliable_tx.end();)
    {
      auto & [packet_id, sent] = *it;
      if (sent.queued || now - sent.last_send_ms < 200)
      {
        ++it;
        continue;
      }
      if (sent.retries >= 8)
      {
        it = r.end_to_end_reliable_tx.erase(it);
        continue;
      }
      sent.retries++;
      sent.queued = true;
      requeue.push_back(packet_id);
      ++it;
    }
    for (const uint64_t packet_id: requeue)
    {
      auto it = r.end_to_end_reliable_tx.find(packet_id);
      if (it == r.end_to_end_reliable_tx.end())
      {
        continue;
      }
      auto pkt = it->second.pkt;
      std::vector<int32_t> sides;
      for (const auto & [_, side]: it->second.pending_destinations)
      {
        sides.push_back(side);
      }
      std::ranges::sort(sides);
      sides.erase(std::ranges::unique(sides).begin(), sides.end());
      if (it->second.tracked_destinations && sides.empty())
      {
        r.end_to_end_reliable_tx.erase(it);
        continue;
      }
      for (const int32_t side: sides)
      {
        enqueue_tx_front(r.tx_queue, r.tx_queue_bytes, {pkt, std::nullopt, side, false});
      }
      if (!it->second.tracked_destinations)
      {
        enqueue_tx_front(r.tx_queue, r.tx_queue_bytes, {pkt, std::nullopt, std::nullopt, false});
      }
      it->second.last_send_ms = now;
      it->second.queued = false;
    }
  }

  void handle_reliable_ack(SedsRouter & r, int32_t side_id, uint32_t ty, uint32_t ack)
  {
    handle_ack_impl(r, side_id, ty, ack);
  }

  void handle_reliable_ack(SedsRelay & r, int32_t side_id, uint32_t ty, uint32_t ack)
  {
    handle_ack_impl(r, side_id, ty, ack);
  }

  bool process_reliable_ingress(SedsRouter & r, int32_t side_id, const FrameInfoLite & frame, const PacketData & pkt,
                                std::span<const uint8_t> wire_bytes)
  {
    return process_reliable_ingress_impl(r, side_id, frame, pkt, wire_bytes);
  }

  bool process_reliable_ingress(SedsRelay & r, int32_t side_id, const FrameInfoLite & frame, const PacketData & pkt,
                                std::span<const uint8_t> wire_bytes)
  {
    return process_reliable_ingress_impl(r, side_id, frame, pkt, wire_bytes);
  }
} // namespace seds
