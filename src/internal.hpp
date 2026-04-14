#pragma once

#include "sedsprintf.h"
#include "discovery.hpp"

#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace seds
{
  constexpr size_t kCompressionThreshold = 128;
  constexpr uint8_t kFlagCompressedPayload = 0x01;
  constexpr uint8_t kFlagCompressedSender = 0x02;
  constexpr uint8_t kReliableFlagAckOnly = 0x01;
  constexpr uint8_t kReliableFlagUnordered = 0x02;
  constexpr uint8_t kReliableFlagUnsequenced = 0x80;
  constexpr size_t kReliableHeaderBytes = 9;
  constexpr size_t kCrcBytes = 4;
  constexpr uint64_t kDiscoveryFastMs = 250;
  constexpr uint64_t kDiscoverySlowMs = 5000;
  constexpr uint64_t kDiscoveryTtlMs = 30000;
  constexpr size_t kStartingQueueBytes = 64;
  constexpr size_t kMaxQueueBytes = 1024 * 50;

  enum class ElementDataType : uint8_t
  {
    NoData,
    Bool,
    UInt8,
    UInt16,
    UInt32,
    UInt64,
    UInt128,
    Int8,
    Int16,
    Int32,
    Int64,
    Int128,
    Float32,
    Float64,
    String,
    Binary,
  };

  enum class MessageClass : uint8_t
  {
    Data,
    Error,
    Warning,
  };

  enum class ReliableMode : uint8_t
  {
    None,
    Ordered,
    Unordered,
  };

  struct TypeInfo
  {
    const char * name;
    size_t elem_size;
    size_t static_count;
    bool dynamic;
    ReliableMode reliable_mode;
    ElementDataType data_type;
    MessageClass message_class;
    bool link_local_only;
    std::vector<uint32_t> endpoints;

    [[nodiscard]] bool reliable() const { return reliable_mode != ReliableMode::None; }
  };

  extern const std::vector<TypeInfo> kTypeInfo;
  extern const uint32_t kEndpointCount;
  extern const std::vector<const char *> kEndpointNames;

  struct PacketData
  {
    uint32_t ty{};
    std::string sender;
    std::vector<uint32_t> endpoints;
    uint64_t timestamp{};
    std::vector<uint8_t> payload;
  };

  struct TelemetryEnvelopeLite
  {
    uint32_t ty{};
    std::vector<uint32_t> endpoints;
    std::string sender;
    uint64_t timestamp_ms{};
  };

  struct ReliableHeaderLite
  {
    uint8_t flags{};
    uint32_t seq{};
    uint32_t ack{};
  };

  struct FrameInfoLite
  {
    TelemetryEnvelopeLite envelope;
    std::optional<ReliableHeaderLite> reliable;

    [[nodiscard]] bool ack_only() const { return reliable.has_value() && (reliable->flags & 0x01u) != 0u; }
  };

  struct LocalEndpoint
  {
    uint32_t endpoint{};
    SedsEndpointHandlerFn packet_handler{};
    SedsSerializedHandlerFn serialized_handler{};
    void * user{};
  };

  struct Side
  {
    std::string name;
    SedsTransmitFn serialized_tx{};
    SedsEndpointHandlerFn packet_tx{};
    void * user{};
    bool reliable_enabled{};
    bool link_local_enabled{};
    bool ingress_enabled{true};
    bool egress_enabled{true};
  };

  struct DiscoveryRoute
  {
    struct SenderState
    {
      std::unordered_set<uint32_t> endpoints;
      std::unordered_set<std::string> timesync_sources;
      std::vector<TopologyBoardNode> topology_boards;
      uint64_t last_seen_ms{};
    };

    std::unordered_set<uint32_t> endpoints;
    std::unordered_set<std::string> timesync_sources;
    uint64_t last_seen_ms{};
    std::unordered_map<std::string, SenderState> announcers;
  };

  inline constexpr bool is_discovery_control_type(const uint32_t ty)
  {
    return ty == SEDS_DT_DISCOVERY_ANNOUNCE || ty == SEDS_DT_DISCOVERY_TIMESYNC_SOURCES ||
           ty == SEDS_DT_DISCOVERY_TOPOLOGY;
  }

  struct RouteKey
  {
    int32_t src_side{-1};
    int32_t dst_side{-1};

    bool operator==(const RouteKey & other) const = default;
  };

  struct TypedRouteKey
  {
    int32_t src_side{-1};
    uint32_t ty{};
    int32_t dst_side{-1};

    bool operator==(const TypedRouteKey & other) const = default;
  };

  struct RouteKeyHash
  {
    size_t operator()(const RouteKey & key) const noexcept;
  };

  struct TypedRouteKeyHash
  {
    size_t operator()(const TypedRouteKey & key) const noexcept;
  };

  struct RoutePolicy
  {
    SedsRouteSelectionMode mode{Seds_RSM_Fanout};
    std::unordered_map<int32_t, uint32_t> weights;
    std::unordered_map<int32_t, uint32_t> priorities;
    size_t rr_counter{};
  };

  struct ReliableTxState
  {
    uint32_t next_seq{1};
    std::optional<std::vector<uint8_t> > inflight_bytes;
    uint32_t inflight_seq{};
    uint64_t last_send_ms{};
    uint32_t retries{};
    std::deque<PacketData> pending;
  };

  struct ReliableRxState
  {
    uint32_t expected_seq{1};
    uint32_t last_ack{};
  };

  struct ReliableReturnRouteState
  {
    int32_t side{-1};
  };

  struct EndToEndReliableSent
  {
    PacketData pkt;
    std::unordered_map<uint64_t, int32_t> pending_destinations;
    bool tracked_destinations{false};
    uint64_t last_send_ms{};
    uint32_t retries{};
    bool queued{false};
  };

  struct TimeSyncRuntime
  {
    bool enabled{false};
    uint32_t role{0};
    uint64_t priority{100};
    uint64_t source_timeout_ms{5000};
    uint64_t announce_interval_ms{1000};
    uint64_t request_interval_ms{1000};
    uint64_t last_announce_ms{};
    uint64_t last_request_ms{};
    bool has_network_time{false};
    uint64_t network_anchor_local_ms{};
    uint64_t network_anchor_unix_ms{};
    std::string current_source;
    uint64_t current_source_priority{UINT64_MAX};

    struct SourceState
    {
      uint64_t priority{};
      uint64_t last_seen_ms{};
      uint64_t last_time_ms{};
    };

    std::unordered_map<std::string, SourceState> sources;
  };

  struct TxItem
  {
    PacketData pkt;
    std::optional<int32_t> src_side;
    std::optional<int32_t> dst_side;
    bool deliver_local{false};
  };

  struct RxItem
  {
    PacketData pkt;
    std::optional<int32_t> src_side;
    std::vector<uint8_t> wire_bytes;
  };

  size_t byte_cost(const PacketData & pkt);

  size_t byte_cost(const TxItem & item);

  size_t byte_cost(const RxItem & item);

  uint64_t default_now_ms();

  bool valid_type(uint32_t ty);

  bool valid_endpoint(uint32_t ep);

  uint32_t crc32_bytes(const uint8_t * data, size_t len);

  void write_uleb128(uint64_t value, std::vector<uint8_t> & out);

  bool read_uleb128(const uint8_t *& cur, const uint8_t * end, uint64_t & out);

  std::vector<uint8_t> endpoint_bitmap(const std::vector<uint32_t> & endpoints);

  std::vector<uint32_t> parse_bitmap(const uint8_t * bitmap, size_t len);

  std::vector<uint8_t> maybe_compress(const uint8_t * data, size_t len, bool & compressed);

  std::vector<uint8_t> maybe_decompress(const uint8_t * data, size_t wire_len, size_t logical_len, bool compressed);

  uint64_t packet_id(const PacketData & pkt);

  bool packet_from_view(const SedsPacketView * view, PacketData & out);

  void fill_view(const PacketData & pkt, SedsPacketView & view);

  std::optional<PacketData> deserialize_packet(const uint8_t * bytes, size_t len);

  std::vector<uint8_t> serialize_packet(const PacketData & pkt);

  std::vector<uint8_t> serialize_packet_with_reliable(const PacketData & pkt, ReliableHeaderLite header);

  std::vector<uint8_t> serialize_reliable_ack(std::string_view sender, uint32_t ty, uint64_t timestamp_ms,
                                              uint32_t ack);

  bool validate_payload(uint32_t ty, size_t bytes);

  std::string error_string(int32_t code);

  int32_t copy_text(std::string_view text, char * buf, size_t buf_len);

  std::optional<FrameInfoLite> peek_frame_info(const uint8_t * bytes, size_t len, bool verify_crc);

  std::optional<size_t> reliable_header_offset(const uint8_t * bytes, size_t len);

  bool rewrite_reliable_header(uint8_t * bytes, size_t len, uint8_t flags, uint32_t seq, uint32_t ack);

  std::optional<uint64_t> packet_id_from_wire(const uint8_t * bytes, size_t len);

  std::string packet_header_string(const SedsPacketView & pkt);

  std::string packet_to_string(const SedsPacketView & pkt);

  bool enqueue_tx(std::deque<TxItem> & queue, size_t & queue_bytes, TxItem item);

  bool enqueue_tx_front(std::deque<TxItem> & queue, size_t & queue_bytes, TxItem item);

  bool enqueue_rx(std::deque<RxItem> & queue, size_t & queue_bytes, RxItem item);

  std::optional<TxItem> pop_tx(std::deque<TxItem> & queue, size_t & queue_bytes);

  std::optional<RxItem> pop_rx(std::deque<RxItem> & queue, size_t & queue_bytes);

  void clear_tx_queue(std::deque<TxItem> & queue, size_t & queue_bytes);

  void clear_rx_queue(std::deque<RxItem> & queue, size_t & queue_bytes);

  template<typename T>
  void append_le(T value, std::vector<uint8_t> & out)
  {
    const auto * ptr = reinterpret_cast<const uint8_t *>(&value);
    out.insert(out.end(), ptr, ptr + sizeof(T));
  }

  template<typename T>
  struct PayloadElementType;

  template<>
  struct PayloadElementType<float>
  {
    static constexpr ElementDataType value = ElementDataType::Float32;
  };

  template<>
  struct PayloadElementType<double>
  {
    static constexpr ElementDataType value = ElementDataType::Float64;
  };

  template<>
  struct PayloadElementType<uint8_t>
  {
    static constexpr ElementDataType value = ElementDataType::UInt8;
  };

  template<>
  struct PayloadElementType<uint16_t>
  {
    static constexpr ElementDataType value = ElementDataType::UInt16;
  };

  template<>
  struct PayloadElementType<uint32_t>
  {
    static constexpr ElementDataType value = ElementDataType::UInt32;
  };

  template<>
  struct PayloadElementType<uint64_t>
  {
    static constexpr ElementDataType value = ElementDataType::UInt64;
  };

  template<>
  struct PayloadElementType<int8_t>
  {
    static constexpr ElementDataType value = ElementDataType::Int8;
  };

  template<>
  struct PayloadElementType<int16_t>
  {
    static constexpr ElementDataType value = ElementDataType::Int16;
  };

  template<>
  struct PayloadElementType<int32_t>
  {
    static constexpr ElementDataType value = ElementDataType::Int32;
  };

  template<>
  struct PayloadElementType<int64_t>
  {
    static constexpr ElementDataType value = ElementDataType::Int64;
  };

  template<>
  struct PayloadElementType<bool>
  {
    static constexpr ElementDataType value = ElementDataType::Bool;
  };

  template<typename T>
  int32_t copy_typed_payload(const SedsPacketView * pkt, T * out, size_t out_elems)
  {
    if (pkt == nullptr)
    {
      return SEDS_BAD_ARG;
    }
    if (!valid_type(pkt->ty) || kTypeInfo[pkt->ty].data_type != PayloadElementType<T>::value)
    {
      return SEDS_TYPE_MISMATCH;
    }
    const size_t need = pkt->payload_len / sizeof(T);
    if (out == nullptr || out_elems < need)
    {
      return static_cast<int32_t>(need);
    }
    std::memcpy(out, pkt->payload, need * sizeof(T));
    return static_cast<int32_t>(need);
  }

  template<>
  inline int32_t copy_typed_payload<bool>(const SedsPacketView * pkt, bool * out, size_t out_elems)
  {
    if (pkt == nullptr)
    {
      return SEDS_BAD_ARG;
    }
    if (!valid_type(pkt->ty) || kTypeInfo[pkt->ty].data_type != PayloadElementType<bool>::value)
    {
      return SEDS_TYPE_MISMATCH;
    }
    const size_t need = pkt->payload_len;
    if (out == nullptr || out_elems < need)
    {
      return static_cast<int32_t>(need);
    }
    for (size_t i = 0; i < need; ++i)
    {
      out[i] = pkt->payload[i] != 0;
    }
    return static_cast<int32_t>(need);
  }

  PacketData make_internal_packet(uint32_t ty, uint64_t ts, std::vector<uint8_t> payload);

  PacketData make_reliable_control_packet(uint32_t control_ty, uint32_t ty, uint32_t seq, uint64_t ts,
                                          std::string_view sender);

  uint64_t sender_hash(std::string_view sender);

  std::vector<uint32_t> local_endpoints_for_error(const std::vector<LocalEndpoint> & locals);

  PacketData make_router_error_packet(const std::vector<LocalEndpoint> & locals, std::string message);

  PacketData make_router_error_packet(std::vector<uint32_t> endpoints, std::string message);

  std::vector<int32_t> apply_policy(RoutePolicy & policy, std::vector<int32_t> sides);

  uint64_t reliable_key(int32_t side_id, uint32_t ty);

  bool is_reliable_type(uint32_t ty);

  bool endpoint_link_local_only(uint32_t ep);

  bool packet_requires_link_local(const PacketData & pkt);

  bool side_accepts_packet(const Side & side, const PacketData & pkt);

  void fill_network_time(uint64_t unix_ms, SedsNetworkTime & out);

  std::optional<uint64_t> network_time_to_unix_ms(int32_t year, uint8_t month, uint8_t day, uint8_t hour,
                                                  uint8_t minute,
                                                  uint8_t second, uint32_t nanosecond);

  PacketData make_e2e_reliable_ack_packet(uint64_t packet_id, uint64_t ts, std::string_view sender);
} // namespace seds

struct SedsOwnedPacket
{
  seds::PacketData pkt;
};

struct SedsOwnedHeader
{
  seds::PacketData pkt;
};

struct SedsRouter
{
  explicit SedsRouter(SedsRouterMode router_mode);

  SedsRouterMode mode;
  SedsNowMsFn now_ms_cb{};
  void * clock_user{};
  std::vector<seds::LocalEndpoint> locals;
  std::vector<seds::Side> sides;
  std::deque<seds::TxItem> tx_queue;
  std::deque<seds::RxItem> rx_queue;
  size_t tx_queue_bytes{};
  size_t rx_queue_bytes{};
  std::deque<uint64_t> recent_ids;
  std::unordered_set<uint64_t> recent_set;
  std::unordered_map<int32_t, seds::DiscoveryRoute> discovery_routes;
  uint64_t discovery_interval_ms{seds::kDiscoveryFastMs};
  uint64_t discovery_next_ms{};
  seds::RoutePolicy local_policy;
  std::unordered_map<int32_t, seds::RoutePolicy> source_policy;
  std::unordered_map<seds::RouteKey, bool, seds::RouteKeyHash> route_overrides;
  std::unordered_map<seds::TypedRouteKey, bool, seds::TypedRouteKeyHash> typed_route_overrides;
  std::unordered_map<uint64_t, seds::ReliableTxState> reliable_tx;
  std::unordered_map<uint64_t, seds::ReliableRxState> reliable_rx;
  std::unordered_map<uint64_t, seds::ReliableReturnRouteState> reliable_return_routes;
  std::unordered_map<uint64_t, seds::EndToEndReliableSent> end_to_end_reliable_tx;
  std::string sender;
  std::string node_sender;
  seds::TimeSyncRuntime timesync;
  mutable std::recursive_mutex mu;

  uint64_t now_ms() const;

  uint64_t current_network_ms() const;
};

struct SedsRelay
{
  SedsNowMsFn now_ms_cb{};
  void * clock_user{};
  std::vector<seds::Side> sides;
  std::deque<seds::TxItem> tx_queue;
  std::deque<seds::RxItem> rx_queue;
  size_t tx_queue_bytes{};
  size_t rx_queue_bytes{};
  std::deque<uint64_t> recent_ids;
  std::unordered_set<uint64_t> recent_set;
  std::unordered_map<int32_t, seds::DiscoveryRoute> discovery_routes;
  uint64_t discovery_interval_ms{seds::kDiscoveryFastMs};
  uint64_t discovery_next_ms{};
  seds::RoutePolicy local_policy;
  std::unordered_map<int32_t, seds::RoutePolicy> source_policy;
  std::unordered_map<seds::RouteKey, bool, seds::RouteKeyHash> route_overrides;
  std::unordered_map<seds::TypedRouteKey, bool, seds::TypedRouteKeyHash> typed_route_overrides;
  std::unordered_map<uint64_t, seds::ReliableTxState> reliable_tx;
  std::unordered_map<uint64_t, seds::ReliableRxState> reliable_rx;
  std::unordered_map<uint64_t, seds::ReliableReturnRouteState> reliable_return_routes;
  std::unordered_map<uint64_t, std::unordered_set<uint64_t> > end_to_end_acked_destinations;
  mutable std::recursive_mutex mu;

  uint64_t now_ms() const;
};

namespace seds
{
  void push_recent(SedsRouter & r, uint64_t id);

  void push_recent(SedsRelay & r, uint64_t id);

  void dispatch_local_from_packet(const PacketData & pkt, const std::vector<LocalEndpoint> & locals);

  SedsResult dispatch_local_packet_handlers(const PacketData & pkt, const std::vector<LocalEndpoint> & locals);

  void dispatch_local_serialized_handlers(const TelemetryEnvelopeLite & env, const uint8_t * bytes, size_t len,
                                          const std::vector<LocalEndpoint> & locals);

  void queue_discovery_packets(SedsRouter & r);

  void queue_discovery_packets(SedsRelay & r);

  std::vector<TopologyBoardNode> advertised_discovery_topology_for_side(const SedsRouter & r, int32_t dst_side);

  std::vector<TopologyBoardNode> advertised_discovery_topology_for_side(const SedsRelay & r, int32_t dst_side);

  void handle_timesync_packet(SedsRouter & r, const PacketData & pkt, std::optional<int32_t> src_side);

  void handle_discovery_packet(SedsRouter & r, const PacketData & pkt, std::optional<int32_t> src_side);

  void handle_discovery_packet(SedsRelay & r, const PacketData & pkt, std::optional<int32_t> src_side);

  void router_receive_impl(SedsRouter & r, PacketData pkt, std::optional<int32_t> src_side);

  void relay_receive_impl(SedsRelay & relay, PacketData pkt, std::optional<int32_t> src_side);

  SedsResult transmit_item(SedsRouter & owner, const TxItem & item);

  SedsResult transmit_item(SedsRelay & owner, const TxItem & item);

  void process_reliable_timeouts(SedsRouter & r);

  void process_reliable_timeouts(SedsRelay & r);

  void handle_reliable_ack(SedsRouter & r, int32_t side_id, uint32_t ty, uint32_t ack);

  void handle_reliable_ack(SedsRelay & r, int32_t side_id, uint32_t ty, uint32_t ack);

  bool process_reliable_ingress(SedsRouter & r, int32_t side_id, const FrameInfoLite & frame);

  bool process_reliable_ingress(SedsRelay & r, int32_t side_id, const FrameInfoLite & frame);

  void process_end_to_end_reliable_timeouts(SedsRouter & r);

  void note_reliable_return_route(SedsRouter & r, int32_t side_id, uint64_t packet_id);

  void note_reliable_return_route(SedsRelay & r, int32_t side_id, uint64_t packet_id);

  void reconcile_end_to_end_reliable_destinations(SedsRouter & r);

  TopologySnapshot export_topology_snapshot(const SedsRouter & r);

  TopologySnapshot export_topology_snapshot(const SedsRelay & r);

  std::string topology_snapshot_to_json(const TopologySnapshot & snap);
} // namespace seds
