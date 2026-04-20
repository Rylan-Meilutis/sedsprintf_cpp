#include "sedsprintf.h"
#include "src/internal.hpp"
#include "src/router.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#undef assert
#define assert(expr) ASSERT_TRUE((expr))

namespace {

constexpr uint32_t kSdCardEndpoint = SEDS_EP_SD_CARD;

struct RouterDeleter {
  void operator()(SedsRouter *router) const {
    if (router != nullptr) {
      seds_router_free(router);
    }
  }
};

struct RelayDeleter {
  void operator()(SedsRelay *relay) const {
    if (relay != nullptr) {
      seds_relay_free(relay);
    }
  }
};

using RouterHandle = std::unique_ptr<SedsRouter, RouterDeleter>;
using RelayHandle = std::unique_ptr<SedsRelay, RelayDeleter>;
using SharedRouterHandle = std::shared_ptr<SedsRouter>;

RouterHandle make_router(SedsRouterMode mode, SedsNowMsFn now_ms_cb = nullptr, void *user = nullptr,
                         const SedsLocalEndpointDesc *handlers = nullptr, size_t n_handlers = 0) {
  return RouterHandle(seds_router_new(mode, now_ms_cb, user, handlers, n_handlers));
}

SharedRouterHandle make_shared_router(SedsRouterMode mode, SedsNowMsFn now_ms_cb = nullptr, void *user = nullptr,
                                      const SedsLocalEndpointDesc *handlers = nullptr, size_t n_handlers = 0) {
  return SharedRouterHandle(seds_router_new(mode, now_ms_cb, user, handlers, n_handlers), RouterDeleter{});
}

RelayHandle make_relay(SedsNowMsFn now_ms_cb = nullptr, void *user = nullptr) {
  return RelayHandle(seds_relay_new(now_ms_cb, user));
}

struct Capture {
  std::vector<std::vector<uint8_t>> frames;
  unsigned packet_hits{0};
};

struct ErrorCapture {
  unsigned total_hits{0};
  unsigned error_hits{0};
  std::string last_error;
};

struct ReenterState {
  SedsRouter *router{};
  bool triggered{false};
  unsigned h1_hits{0};
  unsigned h2_hits{0};
};

SedsResult capture_tx(const uint8_t *bytes, size_t len, void *user) {
  auto *cap = static_cast<Capture *>(user);
  cap->frames.emplace_back(bytes, bytes + len);
  return SEDS_OK;
}

SedsResult count_packet(const SedsPacketView *, void *user) {
  auto *cap = static_cast<Capture *>(user);
  cap->packet_hits++;
  return SEDS_OK;
}

SedsResult capture_error_packet(const SedsPacketView *pkt, void *user) {
  auto *cap = static_cast<ErrorCapture *>(user);
  cap->total_hits++;
  if (pkt != nullptr && pkt->ty == SEDS_DT_TELEMETRY_ERROR) {
    cap->error_hits++;
    std::vector<char> text(static_cast<size_t>(seds_pkt_to_string_len(pkt)));
    if (seds_pkt_to_string(pkt, text.data(), text.size()) == SEDS_OK) {
      cap->last_error.assign(text.data());
    }
  }
  return SEDS_OK;
}

SedsResult reenter_sd_handler(const SedsPacketView *, void *user) {
  auto *state = static_cast<ReenterState *>(user);
  state->h1_hits++;
  if (!state->triggered) {
    state->triggered = true;
    const float chained[] = {9.0f, 8.0f, 7.0f};
    constexpr uint32_t radio_endpoint = SEDS_EP_RADIO;
    const SedsPacketView pkt{
        .ty = SEDS_DT_GPS_DATA,
        .data_size = sizeof(chained),
        .sender = "TEST",
        .sender_len = 4,
        .endpoints = &radio_endpoint,
        .num_endpoints = 1,
        .timestamp = 999,
        .payload = reinterpret_cast<const uint8_t *>(chained),
        .payload_len = sizeof(chained),
    };
    return seds_router_rx_packet_to_queue(state->router, &pkt);
  }
  return SEDS_OK;
}

SedsResult reenter_radio_handler(const SedsPacketView *, void *user) {
  auto *state = static_cast<ReenterState *>(user);
  state->h2_hits++;
  return SEDS_OK;
}

SedsResult capture_serialized_handler(const uint8_t *bytes, size_t len, void *user) {
  auto *cap = static_cast<Capture *>(user);
  cap->frames.emplace_back(bytes, bytes + len);
  return SEDS_OK;
}

struct ManualClock {
  uint64_t now_ms{0};
};

uint64_t read_clock(void *user) { return static_cast<ManualClock *>(user)->now_ms; }

struct StepClockState {
  uint64_t now_ms{0};
  uint64_t step_ms{1};
};

uint64_t read_step_clock(void *user) {
  auto *clock = static_cast<StepClockState *>(user);
  const uint64_t value = clock->now_ms;
  clock->now_ms += clock->step_ms;
  return value;
}

struct TimedCapture {
  ManualClock *clock{};
  std::vector<std::pair<uint64_t, std::vector<uint8_t>>> frames;
};

struct ReliableSeqCapture {
  std::vector<uint32_t> seqs;
};

struct FloatValueCapture {
  std::vector<uint32_t> values;
};

SedsResult timed_capture_tx(const uint8_t *bytes, size_t len, void *user) {
  auto *cap = static_cast<TimedCapture *>(user);
  cap->frames.emplace_back(cap->clock ? cap->clock->now_ms : 0, std::vector<uint8_t>(bytes, bytes + len));
  return SEDS_OK;
}

SedsResult always_fail_tx(const uint8_t *, size_t, void *) { return SEDS_IO; }

SedsResult capture_reliable_seq_handler(const uint8_t *bytes, size_t len, void *user) {
  auto *cap = static_cast<ReliableSeqCapture *>(user);
  const auto frame = seds::peek_frame_info(bytes, len, true);
  if (frame && frame->reliable && !frame->ack_only()) {
    cap->seqs.push_back(frame->reliable->seq);
  }
  return SEDS_OK;
}

SedsResult capture_first_float_handler(const SedsPacketView *pkt, void *user) {
  auto *cap = static_cast<FloatValueCapture *>(user);
  if (pkt == nullptr || pkt->payload_len < sizeof(float)) {
    return SEDS_BAD_ARG;
  }
  float value = 0.0f;
  std::memcpy(&value, pkt->payload, sizeof(value));
  cap->values.push_back(static_cast<uint32_t>(value));
  return SEDS_OK;
}

std::vector<uint8_t> reliable_control_wire(const uint32_t control_ty, const uint32_t ty, const uint32_t seq,
                                           const std::string_view sender = "DST", const uint64_t ts = 0) {
  return seds::serialize_packet(seds::make_reliable_control_packet(control_ty, ty, seq, ts, sender));
}

struct SimBus;

struct SimNode {
  SedsRouter *router{};
  SimBus *bus{};
  uint32_t side_id{};
  unsigned radio_hits{};
  unsigned sd_hits{};
};

struct SimBus {
  std::vector<SimNode *> nodes;
  SedsRelay *relay{};
  uint32_t relay_side{};
  std::deque<std::vector<uint8_t>> pending_from_node;
  std::deque<std::vector<uint8_t>> pending_from_relay;
};

std::vector<uint32_t> decode_discovery_announce_payload(const seds::PacketData &pkt);

SedsResult sim_radio_handler(const SedsPacketView *, void *user) {
  static_cast<SimNode *>(user)->radio_hits++;
  return SEDS_OK;
}

SedsResult sim_sd_handler(const SedsPacketView *, void *user) {
  static_cast<SimNode *>(user)->sd_hits++;
  return SEDS_OK;
}

SedsResult sim_node_tx(const uint8_t *bytes, size_t len, void *user) {
  auto *const node = static_cast<SimNode *>(user);
  node->bus->pending_from_node.emplace_back(bytes, bytes + len);
  return SEDS_OK;
}

SedsResult sim_relay_tx(const uint8_t *bytes, size_t len, void *user) {
  auto *const bus = static_cast<SimBus *>(user);
  bus->pending_from_relay.emplace_back(bytes, bytes + len);
  return SEDS_OK;
}

void pump_bus(SimBus &bus) {
  while (!bus.pending_from_node.empty()) {
    auto frame = std::move(bus.pending_from_node.front());
    bus.pending_from_node.pop_front();
    for (auto *const node : bus.nodes) {
      seds_router_rx_serialized_packet_to_queue_from_side(node->router, node->side_id, frame.data(), frame.size());
    }
    if (bus.relay) {
      seds_relay_rx_serialized_from_side(bus.relay, bus.relay_side, frame.data(), frame.size());
    }
  }
  while (!bus.pending_from_relay.empty()) {
    auto frame = std::move(bus.pending_from_relay.front());
    bus.pending_from_relay.pop_front();
    for (auto *const node : bus.nodes) {
      seds_router_rx_serialized_packet_to_queue_from_side(node->router, node->side_id, frame.data(), frame.size());
    }
  }
}

void test_packet_roundtrip_and_compression() {
  const std::string payload(512, 'A');
  const uint32_t endpoints[] = {SEDS_EP_SD_CARD, SEDS_EP_RADIO};
  const SedsPacketView pkt{
      .ty = SEDS_DT_MESSAGE_DATA,
      .data_size = payload.size(),
      .sender = "TEST",
      .sender_len = 4,
      .endpoints = endpoints,
      .num_endpoints = 2,
      .timestamp = 42,
      .payload = reinterpret_cast<const uint8_t *>(payload.data()),
      .payload_len = payload.size(),
  };

  const int32_t wire_size = seds_pkt_serialize_len(&pkt);
  assert(wire_size > 0);
  assert(static_cast<size_t>(wire_size) < payload.size() + 32);

  std::vector<uint8_t> wire(static_cast<size_t>(wire_size));
  const int32_t written = seds_pkt_serialize(&pkt, wire.data(), wire.size());
  assert(written == wire_size);

  SedsOwnedPacket *owned = seds_pkt_deserialize_owned(wire.data(), wire.size());
  assert(owned != nullptr);
  SedsPacketView view{};
  assert(seds_owned_pkt_view(owned, &view) == SEDS_OK);
  assert(view.ty == SEDS_DT_MESSAGE_DATA);
  assert(view.payload_len == payload.size());
  assert(std::memcmp(view.payload, payload.data(), payload.size()) == 0);
  seds_owned_pkt_free(owned);

  const auto frame = seds::peek_frame_info(wire.data(), wire.size(), true);
  assert(frame.has_value());
  assert(frame->envelope.ty == SEDS_DT_MESSAGE_DATA);
  assert(frame->envelope.sender == "TEST");
  assert(!frame->reliable.has_value());
}

void test_router_c_abi_delivery() {
  Capture local{};
  Capture tx{};
  const SedsLocalEndpointDesc handlers[] = {
      {.endpoint = SEDS_EP_SD_CARD, .packet_handler = count_packet, .serialized_handler = nullptr, .user = &local},
  };
  SedsRouter *const router = seds_router_new(Seds_RM_Sink, nullptr, nullptr, handlers, 1);
  assert(router != nullptr);
  assert(seds_router_add_side_serialized(router, "BUS", 3, capture_tx, &tx, true) >= 0);

  const float gps[] = {1.0f, 2.0f, 3.0f};
  assert(seds_router_log_f32(router, SEDS_DT_GPS_DATA, gps, 3) == SEDS_OK);
  assert(local.packet_hits == 1);
  assert(tx.frames.size() == 1);

  SedsOwnedPacket *owned = seds_pkt_deserialize_owned(tx.frames.front().data(), tx.frames.front().size());
  assert(owned != nullptr);
  SedsPacketView view{};
  assert(seds_owned_pkt_view(owned, &view) == SEDS_OK);
  assert(view.num_endpoints == 2);
  assert(view.endpoints[0] == SEDS_EP_SD_CARD);
  assert(view.endpoints[1] == SEDS_EP_RADIO);
  const auto packet_id = seds::packet_id_from_wire(tx.frames.front().data(), tx.frames.front().size());
  assert(packet_id.has_value());
  seds_owned_pkt_free(owned);
  seds_router_free(router);
}

void test_router_sends_and_receives() {
  Capture local{};
  Capture tx{};
  const SedsLocalEndpointDesc handlers[] = {
      {.endpoint = SEDS_EP_SD_CARD, .packet_handler = count_packet, .serialized_handler = nullptr, .user = &local},
  };
  const auto router = make_router(Seds_RM_Sink, nullptr, nullptr, handlers, 1);
  ASSERT_NE(router, nullptr);
  ASSERT_GE(seds_router_add_side_serialized(router.get(), "BUS", 3, capture_tx, &tx, false), 0);

  const float gps[] = {1.0f, 2.0f, 3.0f};
  ASSERT_EQ(seds_router_log_f32(router.get(), SEDS_DT_GPS_DATA, gps, 3), SEDS_OK);
  ASSERT_EQ(local.packet_hits, 1u);
  ASSERT_EQ(tx.frames.size(), 1u);
}

void test_queued_roundtrip_between_two_routers() {
  Capture a_to_b{};
  Capture b_local{};
  const SedsLocalEndpointDesc b_handlers[] = {
      {.endpoint = SEDS_EP_SD_CARD, .packet_handler = count_packet, .serialized_handler = nullptr, .user = &b_local},
  };
  const auto router_a = make_router(Seds_RM_Sink);
  const auto router_b = make_router(Seds_RM_Sink, nullptr, nullptr, b_handlers, 1);
  ASSERT_NE(router_a, nullptr);
  ASSERT_NE(router_b, nullptr);
  ASSERT_GE(seds_router_add_side_serialized(router_a.get(), "AB", 2, capture_tx, &a_to_b, false), 0);
  ASSERT_GE(seds_router_add_side_serialized(router_b.get(), "BA", 2, capture_tx, nullptr, false), 0);

  const float gps[] = {1.0f, 2.0f, 3.0f};
  ASSERT_EQ(seds_router_log_f32_ex(router_a.get(), SEDS_DT_GPS_DATA, gps, 3, nullptr, 1), SEDS_OK);
  ASSERT_EQ(seds_router_process_tx_queue(router_a.get()), SEDS_OK);
  ASSERT_EQ(a_to_b.frames.size(), 1u);
  ASSERT_EQ(seds_router_rx_serialized_packet_to_queue(router_b.get(), a_to_b.frames[0].data(), a_to_b.frames[0].size()),
            SEDS_OK);
  ASSERT_EQ(seds_router_process_rx_queue(router_b.get()), SEDS_OK);
  ASSERT_EQ(b_local.packet_hits, 1u);
}

void test_queued_self_delivery_via_receive_queue() {
  Capture local{};
  const SedsLocalEndpointDesc handlers[] = {
      {.endpoint = SEDS_EP_SD_CARD, .packet_handler = count_packet, .serialized_handler = nullptr, .user = &local},
  };
  const auto router = make_router(Seds_RM_Sink, nullptr, nullptr, handlers, 1);
  ASSERT_NE(router, nullptr);
  const float gps[] = {4.0f, 5.0f, 6.0f};
  const SedsPacketView pkt{
      .ty = SEDS_DT_GPS_DATA,
      .data_size = sizeof(gps),
      .sender = "SRC",
      .sender_len = 3,
      .endpoints = &kSdCardEndpoint,
      .num_endpoints = 1,
      .timestamp = 0,
      .payload = reinterpret_cast<const uint8_t *>(gps),
      .payload_len = sizeof(gps),
  };
  ASSERT_EQ(seds_router_rx_packet_to_queue(router.get(), &pkt), SEDS_OK);
  ASSERT_EQ(seds_router_process_rx_queue(router.get()), SEDS_OK);
  ASSERT_EQ(local.packet_hits, 1u);
}

void test_local_handler_failure_sends_error_packet_to_other_locals() {
  ErrorCapture errors{};
  const auto failing = [](const SedsPacketView *, void *) -> SedsResult { return SEDS_BAD_ARG; };
  const SedsLocalEndpointDesc handlers[] = {
      {.endpoint = SEDS_EP_SD_CARD, .packet_handler = failing, .serialized_handler = nullptr, .user = nullptr},
      {.endpoint = SEDS_EP_TELEMETRY_ERROR,
       .packet_handler = capture_error_packet,
       .serialized_handler = nullptr,
       .user = &errors},
  };
  const auto router = make_router(Seds_RM_Sink, nullptr, nullptr, handlers, 2);
  ASSERT_NE(router, nullptr);
  const float gps[] = {1.0f, 2.0f, 3.0f};
  const uint32_t endpoints[] = {SEDS_EP_SD_CARD, SEDS_EP_TELEMETRY_ERROR};
  const SedsPacketView pkt{
      .ty = SEDS_DT_GPS_DATA,
      .data_size = sizeof(gps),
      .sender = "SRC",
      .sender_len = 3,
      .endpoints = endpoints,
      .num_endpoints = 2,
      .timestamp = 42,
      .payload = reinterpret_cast<const uint8_t *>(gps),
      .payload_len = sizeof(gps),
  };
  ASSERT_EQ(seds_router_transmit_message(router.get(), &pkt), SEDS_HANDLER_ERROR);
  ASSERT_GE(errors.error_hits, 1u);
  ASSERT_NE(errors.last_error.find("Handler for endpoint SD_CARD failed"), std::string::npos);
}

void test_local_handler_retry_attempts_are_three() {
  std::atomic<unsigned> attempts{0};
  const auto failing = [](const SedsPacketView *, void *user) -> SedsResult {
    auto *count = static_cast<std::atomic<unsigned> *>(user);
    count->fetch_add(1, std::memory_order_seq_cst);
    return SEDS_BAD_ARG;
  };
  const SedsLocalEndpointDesc handlers[] = {
      {.endpoint = SEDS_EP_SD_CARD, .packet_handler = failing, .serialized_handler = nullptr, .user = &attempts},
  };
  const auto router = make_router(Seds_RM_Sink, nullptr, nullptr, handlers, 1);
  ASSERT_NE(router, nullptr);
  const float gps[] = {1.0f, 2.0f, 3.0f};
  const SedsPacketView pkt{
      .ty = SEDS_DT_GPS_DATA,
      .data_size = sizeof(gps),
      .sender = "SRC",
      .sender_len = 3,
      .endpoints = &kSdCardEndpoint,
      .num_endpoints = 1,
      .timestamp = 0,
      .payload = reinterpret_cast<const uint8_t *>(gps),
      .payload_len = sizeof(gps),
  };
  ASSERT_EQ(seds_router_transmit_message(router.get(), &pkt), SEDS_HANDLER_ERROR);
  ASSERT_EQ(attempts.load(std::memory_order_seq_cst), 3u);
}

void test_tx_failure_sends_error_packet_to_all_local_endpoints() {
  ErrorCapture local{};
  const SedsLocalEndpointDesc handlers[] = {
      {.endpoint = SEDS_EP_SD_CARD,
       .packet_handler = capture_error_packet,
       .serialized_handler = nullptr,
       .user = &local},
  };
  const auto router = make_router(Seds_RM_Sink, nullptr, nullptr, handlers, 1);
  ASSERT_NE(router, nullptr);
  ASSERT_GE(seds_router_add_side_serialized(router.get(), "tx", 2, always_fail_tx, nullptr, false), 0);

  const float gps[] = {1.0f, 2.0f, 3.0f};
  const uint32_t endpoints[] = {SEDS_EP_SD_CARD, SEDS_EP_RADIO};
  const SedsPacketView pkt{
      .ty = SEDS_DT_GPS_DATA,
      .data_size = sizeof(gps),
      .sender = "SRC",
      .sender_len = 3,
      .endpoints = endpoints,
      .num_endpoints = 2,
      .timestamp = 31415,
      .payload = reinterpret_cast<const uint8_t *>(gps),
      .payload_len = sizeof(gps),
  };
  ASSERT_EQ(seds_router_transmit_message(router.get(), &pkt), SEDS_HANDLER_ERROR);
  ASSERT_GE(local.error_hits, 1u);
  ASSERT_NE(local.last_error.find("TX handler failed"), std::string::npos);
}

void test_receive_serialized_preserves_original_wire_for_local_serialized_handlers() {
  Capture local_packet{};
  Capture local_serialized{};
  Capture ack_tx{};
  const SedsLocalEndpointDesc handlers[] = {
      {.endpoint = SEDS_EP_SD_CARD,
       .packet_handler = count_packet,
       .serialized_handler = capture_serialized_handler,
       .user = &local_packet},
      {.endpoint = SEDS_EP_RADIO,
       .packet_handler = nullptr,
       .serialized_handler = capture_serialized_handler,
       .user = &local_serialized},
  };
  const auto router = make_router(Seds_RM_Sink, nullptr, nullptr, handlers, 2);
  ASSERT_NE(router, nullptr);
  const int32_t side_id = seds_router_add_side_serialized(router.get(), "BUS", 3, capture_tx, &ack_tx, true);
  ASSERT_GE(side_id, 0);

  const float gps[] = {1.0f, 2.0f, 3.0f};
  seds::PacketData pkt;
  pkt.ty = SEDS_DT_GPS_DATA;
  pkt.sender = "REMOTE";
  pkt.endpoints = {SEDS_EP_SD_CARD, SEDS_EP_RADIO};
  pkt.timestamp = 7;
  pkt.payload.assign(reinterpret_cast<const uint8_t *>(gps), reinterpret_cast<const uint8_t *>(gps) + sizeof(gps));
  const auto wire = seds::serialize_packet_with_reliable(pkt, seds::ReliableHeaderLite{0, 1, 0});

  ASSERT_EQ(
      seds_router_receive_serialized_from_side(router.get(), static_cast<uint32_t>(side_id), wire.data(), wire.size()),
      SEDS_OK);
  ASSERT_EQ(seds_router_process_tx_queue(router.get()), SEDS_OK);
  ASSERT_EQ(local_packet.packet_hits, 1u);
  ASSERT_EQ(local_serialized.frames.size(), 1u);
  ASSERT_EQ(local_serialized.frames.front(), wire);
  ASSERT_FALSE(ack_tx.frames.empty());
}

void test_serialized_only_handlers_do_not_deserialize() {
  Capture local{};
  const SedsLocalEndpointDesc handlers[] = {
      {.endpoint = SEDS_EP_SD_CARD,
       .packet_handler = nullptr,
       .serialized_handler = capture_serialized_handler,
       .user = &local},
  };
  const auto router = make_router(Seds_RM_Sink, nullptr, nullptr, handlers, 1);
  ASSERT_NE(router, nullptr);

  const float gps[] = {1.0f, 2.0f, 3.0f};
  const SedsPacketView pkt{
      .ty = SEDS_DT_GPS_DATA,
      .data_size = sizeof(gps),
      .sender = "TEST",
      .sender_len = 4,
      .endpoints = &kSdCardEndpoint,
      .num_endpoints = 1,
      .timestamp = 123,
      .payload = reinterpret_cast<const uint8_t *>(gps),
      .payload_len = sizeof(gps),
  };
  std::vector<uint8_t> wire(static_cast<size_t>(seds_pkt_serialize_len(&pkt)));
  ASSERT_EQ(seds_pkt_serialize(&pkt, wire.data(), wire.size()), static_cast<int32_t>(wire.size()));

  ASSERT_EQ(seds_router_receive_serialized(router.get(), wire.data(), wire.size()), SEDS_OK);
  ASSERT_EQ(local.frames.size(), 1u);
  ASSERT_EQ(local.frames.front(), wire);
  ASSERT_EQ(local.packet_hits, 0u);
}

void test_packet_and_serialized_handlers_fan_out_once() {
  Capture packet_local{};
  Capture serialized_local{};
  const SedsLocalEndpointDesc handlers[] = {
      {.endpoint = SEDS_EP_SD_CARD,
       .packet_handler = count_packet,
       .serialized_handler = nullptr,
       .user = &packet_local},
      {.endpoint = SEDS_EP_RADIO,
       .packet_handler = nullptr,
       .serialized_handler = capture_serialized_handler,
       .user = &serialized_local},
  };
  const auto router = make_router(Seds_RM_Sink, nullptr, nullptr, handlers, 2);
  ASSERT_NE(router, nullptr);

  const float gps[] = {1.0f, 2.0f, 3.0f};
  const uint32_t endpoints[] = {SEDS_EP_SD_CARD, SEDS_EP_RADIO};
  const SedsPacketView pkt{
      .ty = SEDS_DT_GPS_DATA,
      .data_size = sizeof(gps),
      .sender = "TEST",
      .sender_len = 4,
      .endpoints = endpoints,
      .num_endpoints = 2,
      .timestamp = 5,
      .payload = reinterpret_cast<const uint8_t *>(gps),
      .payload_len = sizeof(gps),
  };
  std::vector<uint8_t> wire(static_cast<size_t>(seds_pkt_serialize_len(&pkt)));
  ASSERT_EQ(seds_pkt_serialize(&pkt, wire.data(), wire.size()), static_cast<int32_t>(wire.size()));

  ASSERT_EQ(seds_router_receive_serialized(router.get(), wire.data(), wire.size()), SEDS_OK);
  ASSERT_EQ(packet_local.packet_hits, 1u);
  ASSERT_EQ(serialized_local.frames.size(), 1u);
  ASSERT_EQ(serialized_local.frames.front(), wire);
}

void test_send_avoids_tx_when_only_local_packet_handlers_exist() {
  Capture local{};
  Capture tx{};
  const SedsLocalEndpointDesc handlers[] = {
      {.endpoint = SEDS_EP_SD_CARD, .packet_handler = count_packet, .serialized_handler = nullptr, .user = &local},
  };
  const auto router = make_router(Seds_RM_Sink, nullptr, nullptr, handlers, 1);
  ASSERT_NE(router, nullptr);
  ASSERT_GE(seds_router_add_side_serialized(router.get(), "tx", 2, capture_tx, &tx, false), 0);

  const float gps[] = {1.0f, 2.0f, 3.0f};
  const SedsPacketView pkt{
      .ty = SEDS_DT_GPS_DATA,
      .data_size = sizeof(gps),
      .sender = "TEST",
      .sender_len = 4,
      .endpoints = &kSdCardEndpoint,
      .num_endpoints = 1,
      .timestamp = 0,
      .payload = reinterpret_cast<const uint8_t *>(gps),
      .payload_len = sizeof(gps),
  };
  ASSERT_EQ(seds_router_transmit_message(router.get(), &pkt), SEDS_OK);
  ASSERT_EQ(local.packet_hits, 1u);
  ASSERT_TRUE(tx.frames.empty());
}

void test_receive_direct_packet_invokes_handlers() {
  Capture local{};
  const SedsLocalEndpointDesc handlers[] = {
      {.endpoint = SEDS_EP_SD_CARD, .packet_handler = count_packet, .serialized_handler = nullptr, .user = &local},
  };
  const auto router = make_router(Seds_RM_Sink, nullptr, nullptr, handlers, 1);
  ASSERT_NE(router, nullptr);

  const float gps[] = {0.5f, 0.5f, 0.5f};
  const SedsPacketView pkt{
      .ty = SEDS_DT_GPS_DATA,
      .data_size = sizeof(gps),
      .sender = "TEST",
      .sender_len = 4,
      .endpoints = &kSdCardEndpoint,
      .num_endpoints = 1,
      .timestamp = 0,
      .payload = reinterpret_cast<const uint8_t *>(gps),
      .payload_len = sizeof(gps),
  };
  ASSERT_EQ(seds_router_receive(router.get(), &pkt), SEDS_OK);
  ASSERT_EQ(local.packet_hits, 1u);
}

void test_receive_serialized_queue_delivers_to_serialized_handlers() {
  Capture local{};
  const SedsLocalEndpointDesc handlers[] = {
      {.endpoint = SEDS_EP_SD_CARD,
       .packet_handler = nullptr,
       .serialized_handler = capture_serialized_handler,
       .user = &local},
  };
  const auto router = make_router(Seds_RM_Sink, nullptr, nullptr, handlers, 1);
  ASSERT_NE(router, nullptr);

  const float gps[] = {1.0f, 2.0f, 3.0f};
  const SedsPacketView pkt{
      .ty = SEDS_DT_GPS_DATA,
      .data_size = sizeof(gps),
      .sender = "TEST",
      .sender_len = 4,
      .endpoints = &kSdCardEndpoint,
      .num_endpoints = 1,
      .timestamp = 0,
      .payload = reinterpret_cast<const uint8_t *>(gps),
      .payload_len = sizeof(gps),
  };
  std::vector<uint8_t> wire(static_cast<size_t>(seds_pkt_serialize_len(&pkt)));
  ASSERT_EQ(seds_pkt_serialize(&pkt, wire.data(), wire.size()), static_cast<int32_t>(wire.size()));

  ASSERT_EQ(seds_router_rx_serialized_packet_to_queue(router.get(), wire.data(), wire.size()), SEDS_OK);
  ASSERT_EQ(seds_router_process_rx_queue(router.get()), SEDS_OK);
  ASSERT_EQ(local.frames.size(), 1u);
  ASSERT_EQ(local.frames.front(), wire);
}

void test_tx_failure_emits_local_error_packet() {
  ErrorCapture local{};
  const SedsLocalEndpointDesc handlers[] = {
      {.endpoint = SEDS_EP_SD_CARD,
       .packet_handler = capture_error_packet,
       .serialized_handler = nullptr,
       .user = &local},
  };
  const auto router = make_router(Seds_RM_Sink, nullptr, nullptr, handlers, 1);
  ASSERT_NE(router, nullptr);
  ASSERT_GE(seds_router_add_side_serialized(router.get(), "tx", 2, always_fail_tx, nullptr, false), 0);

  const float gps[] = {1.0f, 2.0f, 3.0f};
  ASSERT_EQ(seds_router_log_bytes_ex(router.get(), SEDS_DT_GPS_DATA, reinterpret_cast<const uint8_t *>(gps),
                                     sizeof(gps), nullptr, 0),
            SEDS_HANDLER_ERROR);
  ASSERT_GE(local.total_hits, 2u);
  ASSERT_EQ(local.error_hits, 1u);
  ASSERT_NE(local.last_error.find("Type: TELEMETRY_ERROR"), std::string::npos);
  ASSERT_NE(local.last_error.find("Error: ("), std::string::npos);
}

void test_process_all_queues_handles_u64_wraparound() {
  StepClockState clock{UINT64_MAX - 1, 2};
  Capture tx{};
  Capture local{};
  const SedsLocalEndpointDesc handlers[] = {
      {.endpoint = SEDS_EP_SD_CARD, .packet_handler = count_packet, .serialized_handler = nullptr, .user = &local},
  };
  const auto router = make_router(Seds_RM_Sink, read_step_clock, &clock, handlers, 1);
  ASSERT_NE(router, nullptr);
  ASSERT_GE(seds_router_add_side_serialized(router.get(), "tx", 2, capture_tx, &tx, false), 0);

  const float a[] = {1.0f, 2.0f, 3.0f};
  const float b[] = {4.0f, 5.0f, 6.0f};
  ASSERT_EQ(seds_router_log_bytes_ex(router.get(), SEDS_DT_GPS_DATA, reinterpret_cast<const uint8_t *>(a), sizeof(a),
                                     nullptr, 1),
            SEDS_OK);

  SedsPacketView rx_pkt{
      .ty = SEDS_DT_GPS_DATA,
      .data_size = sizeof(b),
      .sender = "TEST",
      .sender_len = 4,
      .endpoints = &kSdCardEndpoint,
      .num_endpoints = 1,
      .timestamp = 7,
      .payload = reinterpret_cast<const uint8_t *>(b),
      .payload_len = sizeof(b),
  };
  ASSERT_EQ(seds_router_rx_packet_to_queue(router.get(), &rx_pkt), SEDS_OK);

  ASSERT_EQ(seds_router_process_all_queues_with_timeout(router.get(), 1), SEDS_OK);
  ASSERT_LE(tx.frames.size(), 1u);
  ASSERT_LE(local.packet_hits, 2u);
  ASSERT_GE(tx.frames.size() + local.packet_hits, 1u);
}

void test_process_all_queues_timeout_zero_drains_fully() {
  StepClockState clock{0, 1};
  Capture tx{};
  Capture local{};
  const SedsLocalEndpointDesc handlers[] = {
      {.endpoint = SEDS_EP_SD_CARD, .packet_handler = count_packet, .serialized_handler = nullptr, .user = &local},
  };
  const auto router = make_router(Seds_RM_Sink, read_step_clock, &clock, handlers, 1);
  ASSERT_NE(router, nullptr);
  ASSERT_GE(seds_router_add_side_serialized(router.get(), "tx", 2, capture_tx, &tx, false), 0);

  for (size_t i = 0; i < 3; ++i) {
    const float payload[] = {1.0f + static_cast<float>(i), 2.0f, 3.0f};
    ASSERT_EQ(seds_router_log_bytes_ex(router.get(), SEDS_DT_GPS_DATA, reinterpret_cast<const uint8_t *>(payload),
                                       sizeof(payload), nullptr, 1),
              SEDS_OK);
  }
  for (uint64_t i = 0; i < 2; ++i) {
    const float payload[] = {9.0f + static_cast<float>(i), 8.0f, 7.0f};
    const SedsPacketView pkt{
        .ty = SEDS_DT_GPS_DATA,
        .data_size = sizeof(payload),
        .sender = "TEST",
        .sender_len = 4,
        .endpoints = &kSdCardEndpoint,
        .num_endpoints = 1,
        .timestamp = 123 + i,
        .payload = reinterpret_cast<const uint8_t *>(payload),
        .payload_len = sizeof(payload),
    };
    ASSERT_EQ(seds_router_rx_packet_to_queue(router.get(), &pkt), SEDS_OK);
  }

  ASSERT_EQ(seds_router_process_all_queues_with_timeout(router.get(), 0), SEDS_OK);
  ASSERT_EQ(tx.frames.size(), 3u);
  ASSERT_EQ(local.packet_hits, 5u);
}

void test_process_all_queues_respects_nonzero_timeout_budget_one_receive_one_send() {
  Capture tx{};
  Capture local{};
  const SedsLocalEndpointDesc handlers[] = {
      {.endpoint = SEDS_EP_SD_CARD, .packet_handler = count_packet, .serialized_handler = nullptr, .user = &local},
  };
  const auto router = make_router(Seds_RM_Sink, nullptr, nullptr, handlers, 1);
  ASSERT_NE(router, nullptr);
  ASSERT_GE(seds_router_add_side_serialized(router.get(), "tx", 2, capture_tx, &tx, false), 0);

  for (uint64_t i = 0; i < 5; ++i) {
    const float tx_payload[] = {1.0f + static_cast<float>(i), 2.0f, 3.0f};
    ASSERT_EQ(seds_router_log_bytes_ex(router.get(), SEDS_DT_GPS_DATA, reinterpret_cast<const uint8_t *>(tx_payload),
                                       sizeof(tx_payload), nullptr, 1),
              SEDS_OK);

    const float rx_payload[] = {4.0f + static_cast<float>(i), 5.0f, 6.0f};
    const SedsPacketView pkt{
        .ty = SEDS_DT_GPS_DATA,
        .data_size = sizeof(rx_payload),
        .sender = "TEST",
        .sender_len = 4,
        .endpoints = &kSdCardEndpoint,
        .num_endpoints = 1,
        .timestamp = 1 + i,
        .payload = reinterpret_cast<const uint8_t *>(rx_payload),
        .payload_len = sizeof(rx_payload),
    };
    ASSERT_EQ(seds_router_rx_packet_to_queue(router.get(), &pkt), SEDS_OK);
  }

  ASSERT_EQ(seds_router_process_all_queues_with_timeout(router.get(), 5), SEDS_OK);
  ASSERT_GT(tx.frames.size() + local.packet_hits, 0u);
  ASSERT_LE(tx.frames.size(), 5u);
  ASSERT_LE(local.packet_hits, 10u);

  ASSERT_EQ(seds_router_process_all_queues_with_timeout(router.get(), 0), SEDS_OK);
  ASSERT_EQ(tx.frames.size(), 5u);
  ASSERT_EQ(local.packet_hits, 10u);
}

void test_process_all_queues_respects_nonzero_timeout_budget_two_receive_one_send() {
  StepClockState clock{0, 5};
  Capture tx{};
  Capture local{};
  const SedsLocalEndpointDesc handlers[] = {
      {.endpoint = SEDS_EP_SD_CARD, .packet_handler = count_packet, .serialized_handler = nullptr, .user = &local},
  };
  const auto router = make_router(Seds_RM_Sink, read_step_clock, &clock, handlers, 1);
  ASSERT_NE(router, nullptr);
  ASSERT_GE(seds_router_add_side_serialized(router.get(), "tx", 2, capture_tx, &tx, false), 0);

  for (uint64_t i = 0; i < 5; ++i) {
    const float tx_payload[] = {1.0f + static_cast<float>(i), 2.0f, 3.0f};
    ASSERT_EQ(seds_router_log_bytes_ex(router.get(), SEDS_DT_GPS_DATA, reinterpret_cast<const uint8_t *>(tx_payload),
                                       sizeof(tx_payload), nullptr, 1),
              SEDS_OK);

    const float rx_payload[] = {4.0f + static_cast<float>(i), 5.0f, 6.0f};
    const SedsPacketView pkt{
        .ty = SEDS_DT_GPS_DATA,
        .data_size = sizeof(rx_payload),
        .sender = "TEST",
        .sender_len = 4,
        .endpoints = &kSdCardEndpoint,
        .num_endpoints = 1,
        .timestamp = 1 + i,
        .payload = reinterpret_cast<const uint8_t *>(rx_payload),
        .payload_len = sizeof(rx_payload),
    };
    ASSERT_EQ(seds_router_rx_packet_to_queue(router.get(), &pkt), SEDS_OK);
  }

  ASSERT_EQ(seds_router_process_all_queues_with_timeout(router.get(), 10), SEDS_OK);
  ASSERT_EQ(tx.frames.size(), 1u);
  ASSERT_EQ(local.packet_hits, 2u);

  ASSERT_EQ(seds_router_process_all_queues_with_timeout(router.get(), 0), SEDS_OK);
  ASSERT_EQ(tx.frames.size(), 5u);
  ASSERT_EQ(local.packet_hits, 10u);
}

SedsResult slow_packet_tx(const SedsPacketView *, void *user) {
  const auto *state = static_cast<std::pair<ManualClock *, Capture *> *>(user);
  state->second->packet_hits++;
  state->first->now_ms = 2;
  return SEDS_OK;
}

void test_process_all_queues_timeout_does_not_starve_rx_after_slow_tx() {
  ManualClock clock{};
  Capture remote{};
  const auto router = make_router(Seds_RM_Sink, read_clock, &clock);
  ASSERT_NE(router, nullptr);

  std::pair slow_state{&clock, &remote};
  ASSERT_GE(seds_router_add_side_packet(router.get(), "REMOTE", 6, slow_packet_tx, &slow_state, false), 0);

  const float gps[] = {1.0f, 2.0f, 3.0f};
  ASSERT_EQ(seds_router_log_bytes_ex(router.get(), SEDS_DT_GPS_DATA, reinterpret_cast<const uint8_t *>(gps),
                                     sizeof(gps), nullptr, 1),
            SEDS_OK);

  seds::PacketData discovery = seds::make_internal_packet(SEDS_DT_DISCOVERY_ANNOUNCE, 0, {});
  discovery.sender = "REMOTE_NODE";
  discovery.endpoints = {SEDS_EP_DISCOVERY};
  seds::append_le<uint32_t>(SEDS_EP_RADIO, discovery.payload);
  const auto wire = seds::serialize_packet(discovery);
  ASSERT_EQ(seds_router_rx_serialized_packet_to_queue_from_side(router.get(), 0, wire.data(), wire.size()), SEDS_OK);

  ASSERT_EQ(seds_router_process_all_queues_with_timeout(router.get(), 2), SEDS_OK);
  ASSERT_EQ(remote.packet_hits, 1u);
  ASSERT_EQ(router->discovery_routes.size(), 1u);
  ASSERT_TRUE(router->discovery_routes.count(0) != 0u);
  ASSERT_TRUE(router->discovery_routes[0].endpoints.count(SEDS_EP_RADIO) != 0u);
}

void test_handler_can_reenter_router_without_deadlock() {
  ReenterState state{};
  const SedsLocalEndpointDesc handlers[] = {
      {.endpoint = SEDS_EP_SD_CARD,
       .packet_handler = reenter_sd_handler,
       .serialized_handler = nullptr,
       .user = &state},
      {.endpoint = SEDS_EP_RADIO,
       .packet_handler = reenter_radio_handler,
       .serialized_handler = nullptr,
       .user = &state},
  };
  const auto router = make_router(Seds_RM_Sink, nullptr, nullptr, handlers, 2);
  ASSERT_NE(router, nullptr);
  state.router = router.get();

  const float first[] = {1.0f, 2.0f, 3.0f};
  const SedsPacketView pkt{
      .ty = SEDS_DT_GPS_DATA,
      .data_size = sizeof(first),
      .sender = "TEST",
      .sender_len = 4,
      .endpoints = &kSdCardEndpoint,
      .num_endpoints = 1,
      .timestamp = 100,
      .payload = reinterpret_cast<const uint8_t *>(first),
      .payload_len = sizeof(first),
  };
  ASSERT_EQ(seds_router_rx_packet_to_queue(router.get(), &pkt), SEDS_OK);
  ASSERT_EQ(seds_router_process_rx_queue(router.get()), SEDS_OK);
  ASSERT_EQ(seds_router_process_rx_queue(router.get()), SEDS_OK);
  ASSERT_EQ(state.h1_hits, 1u);
  ASSERT_EQ(state.h2_hits, 1u);
}

void test_concurrent_receive_serialized_is_thread_safe() {
  constexpr size_t kThreads = 4;
  constexpr size_t kItersPerThread = 50;
  constexpr size_t kTotal = kThreads * kItersPerThread;

  std::atomic<size_t> hits{0};
  const SedsLocalEndpointDesc handlers[] = {
      {.endpoint = SEDS_EP_SD_CARD,
       .packet_handler = [](const SedsPacketView *, void *user) -> SedsResult {
         static_cast<std::atomic<size_t> *>(user)->fetch_add(1, std::memory_order_seq_cst);
         return SEDS_OK;
       },
       .serialized_handler = nullptr,
       .user = &hits},
  };
  auto router = make_shared_router(Seds_RM_Sink, nullptr, nullptr, handlers, 1);
  ASSERT_NE(router, nullptr);

  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (size_t tid = 0; tid < kThreads; ++tid) {
    threads.emplace_back([router, tid]() {
      for (size_t i = 0; i < kItersPerThread; ++i) {
        const uint64_t idx =
            static_cast<uint64_t>(tid) * static_cast<uint64_t>(kItersPerThread) + static_cast<uint64_t>(i);
        const float payload[] = {1.0f + static_cast<float>(idx) * 0.001f, 2.0f, 3.0f};
        const SedsPacketView pkt{
            .ty = SEDS_DT_GPS_DATA,
            .data_size = sizeof(payload),
            .sender = "TEST",
            .sender_len = 4,
            .endpoints = &kSdCardEndpoint,
            .num_endpoints = 1,
            .timestamp = idx,
            .payload = reinterpret_cast<const uint8_t *>(payload),
            .payload_len = sizeof(payload),
        };
        std::vector<uint8_t> wire(static_cast<size_t>(seds_pkt_serialize_len(&pkt)));
        ASSERT_EQ(seds_pkt_serialize(&pkt, wire.data(), wire.size()), static_cast<int32_t>(wire.size()));
        ASSERT_EQ(seds_router_receive_serialized(router.get(), wire.data(), wire.size()), SEDS_OK);
      }
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }
  ASSERT_EQ(hits.load(std::memory_order_seq_cst), kTotal);
}

void test_concurrent_logging_and_processing_is_thread_safe() {
  constexpr size_t kIters = 200;
  std::atomic<size_t> tx_count{0};
  std::atomic<size_t> rx_count{0};
  const SedsLocalEndpointDesc handlers[] = {
      {.endpoint = SEDS_EP_SD_CARD,
       .packet_handler = [](const SedsPacketView *, void *user) -> SedsResult {
         static_cast<std::atomic<size_t> *>(user)->fetch_add(1, std::memory_order_seq_cst);
         return SEDS_OK;
       },
       .serialized_handler = nullptr,
       .user = &rx_count},
  };
  auto router = make_shared_router(Seds_RM_Sink, nullptr, nullptr, handlers, 1);
  ASSERT_NE(router, nullptr);
  ASSERT_GE(seds_router_add_side_serialized(
                router.get(), "tx", 2,
                [](const uint8_t *bytes, size_t len, void *user) -> SedsResult {
                  EXPECT_NE(bytes, nullptr);
                  EXPECT_GT(len, 0u);
                  static_cast<std::atomic<size_t> *>(user)->fetch_add(1, std::memory_order_seq_cst);
                  return SEDS_OK;
                },
                &tx_count, false),
            0);

  auto logger = std::thread([router]() {
    for (size_t i = 0; i < kIters; ++i) {
      const float payload[] = {1.0f, 5.9f + static_cast<float>(i), 3.0f};
      ASSERT_EQ(seds_router_log_bytes_ex(router.get(), SEDS_DT_GPS_DATA, reinterpret_cast<const uint8_t *>(payload),
                                         sizeof(payload), nullptr, 1),
                SEDS_OK);
    }
  });

  auto drainer = std::thread([router, &rx_count]() {
    for (size_t attempts = 0; attempts < 10000 && rx_count.load(std::memory_order_seq_cst) < kIters; ++attempts) {
      ASSERT_EQ(seds_router_process_all_queues(router.get()), SEDS_OK);
      std::this_thread::yield();
    }
  });

  logger.join();
  drainer.join();
  ASSERT_EQ(seds_router_process_all_queues(router.get()), SEDS_OK);

  ASSERT_EQ(rx_count.load(std::memory_order_seq_cst), kIters);
  ASSERT_EQ(tx_count.load(std::memory_order_seq_cst), kIters);
}

void test_concurrent_log_receive_and_process_mix_is_thread_safe() {
  constexpr size_t kLogIters = 100;
  constexpr size_t kRxIters = 100;
  std::atomic<size_t> tx_count{0};
  std::atomic<size_t> rx_count{0};
  const SedsLocalEndpointDesc handlers[] = {
      {.endpoint = SEDS_EP_SD_CARD,
       .packet_handler = [](const SedsPacketView *, void *user) -> SedsResult {
         static_cast<std::atomic<size_t> *>(user)->fetch_add(1, std::memory_order_seq_cst);
         return SEDS_OK;
       },
       .serialized_handler = nullptr,
       .user = &rx_count},
  };
  auto router = make_shared_router(Seds_RM_Sink, nullptr, nullptr, handlers, 1);
  ASSERT_NE(router, nullptr);
  ASSERT_GE(seds_router_add_side_serialized(
                router.get(), "tx", 2,
                [](const uint8_t *bytes, size_t len, void *user) -> SedsResult {
                  EXPECT_NE(bytes, nullptr);
                  EXPECT_GT(len, 0u);
                  static_cast<std::atomic<size_t> *>(user)->fetch_add(1, std::memory_order_seq_cst);
                  return SEDS_OK;
                },
                &tx_count, false),
            0);

  auto logger = std::thread([router]() {
    for (size_t i = 0; i < kLogIters; ++i) {
      const float payload[] = {1.0f + static_cast<float>(i) * 0.01f, 2.0f, 3.0f};
      ASSERT_EQ(seds_router_log_bytes_ex(router.get(), SEDS_DT_GPS_DATA, reinterpret_cast<const uint8_t *>(payload),
                                         sizeof(payload), nullptr, 1),
                SEDS_OK);
    }
  });

  auto rx_thread = std::thread([router]() {
    for (size_t i = 0; i < kRxIters; ++i) {
      const float payload[] = {4.0f + static_cast<float>(i) * 0.01f, 5.0f, 6.0f};
      const SedsPacketView pkt{
          .ty = SEDS_DT_GPS_DATA,
          .data_size = sizeof(payload),
          .sender = "TEST",
          .sender_len = 4,
          .endpoints = &kSdCardEndpoint,
          .num_endpoints = 1,
          .timestamp = static_cast<uint64_t>(i),
          .payload = reinterpret_cast<const uint8_t *>(payload),
          .payload_len = sizeof(payload),
      };
      ASSERT_EQ(seds_router_rx_packet_to_queue(router.get(), &pkt), SEDS_OK);
    }
  });

  auto processor = std::thread([router, &rx_count]() {
    for (size_t attempts = 0; attempts < 10000 && rx_count.load(std::memory_order_seq_cst) < kLogIters + kRxIters;
         ++attempts) {
      ASSERT_EQ(seds_router_process_all_queues(router.get()), SEDS_OK);
      std::this_thread::yield();
    }
  });

  logger.join();
  rx_thread.join();
  processor.join();
  for (size_t attempts = 0; attempts < 1000 && rx_count.load(std::memory_order_seq_cst) < kLogIters + kRxIters;
       ++attempts) {
    ASSERT_EQ(seds_router_process_all_queues(router.get()), SEDS_OK);
    std::this_thread::yield();
  }
  ASSERT_EQ(seds_router_process_all_queues(router.get()), SEDS_OK);

  ASSERT_EQ(tx_count.load(std::memory_order_seq_cst), kLogIters);
  ASSERT_EQ(rx_count.load(std::memory_order_seq_cst), kLogIters + kRxIters);
}

void test_packet_string_formatting() {
  const std::string payload = "hello";
  const uint32_t endpoints[] = {SEDS_EP_SD_CARD, SEDS_EP_RADIO};
  const SedsPacketView pkt{
      .ty = SEDS_DT_MESSAGE_DATA,
      .data_size = payload.size(),
      .sender = "TEST",
      .sender_len = 4,
      .endpoints = endpoints,
      .num_endpoints = 2,
      .timestamp = 1234,
      .payload = reinterpret_cast<const uint8_t *>(payload.data()),
      .payload_len = payload.size(),
  };

  std::vector<char> header(static_cast<size_t>(seds_pkt_header_string_len(&pkt)));
  std::vector<char> full(static_cast<size_t>(seds_pkt_to_string_len(&pkt)));
  assert(seds_pkt_header_string(&pkt, header.data(), header.size()) == SEDS_OK);
  assert(seds_pkt_to_string(&pkt, full.data(), full.size()) == SEDS_OK);

  const std::string header_text(header.data());
  const std::string full_text(full.data());
  assert(header_text.find("Type: MESSAGE_DATA") != std::string::npos);
  assert(header_text.find("Endpoints: [SD_CARD, RADIO]") != std::string::npos);
  assert(header_text.find("Timestamp: 1234 (1s 234ms)") != std::string::npos);
  assert(full_text.find("Data: (\"hello\")") != std::string::npos);

  const float gps[] = {1.0f, 2.5f, 3.0f};
  SedsPacketView numeric{
      .ty = SEDS_DT_GPS_DATA,
      .data_size = sizeof(gps),
      .sender = "NUM",
      .sender_len = 3,
      .endpoints = endpoints,
      .num_endpoints = 2,
      .timestamp = 1000,
      .payload = reinterpret_cast<const uint8_t *>(gps),
      .payload_len = sizeof(gps),
  };
  std::vector<char> numeric_text(static_cast<size_t>(seds_pkt_to_string_len(&numeric)));
  assert(seds_pkt_to_string(&numeric, numeric_text.data(), numeric_text.size()) == SEDS_OK);
  assert(std::string(numeric_text.data()).find("Data: (1.00000000, 2.50000000, 3.00000000)") != std::string::npos);

  SedsPacketView epoch_string{
      .ty = SEDS_DT_MESSAGE_DATA,
      .data_size = payload.size(),
      .sender = "UTC",
      .sender_len = 3,
      .endpoints = endpoints,
      .num_endpoints = 2,
      .timestamp = 1'700'000'000'123ull,
      .payload = reinterpret_cast<const uint8_t *>(payload.data()),
      .payload_len = payload.size(),
  };
  std::vector<char> epoch_text(static_cast<size_t>(seds_pkt_to_string_len(&epoch_string)));
  assert(seds_pkt_to_string(&epoch_string, epoch_text.data(), epoch_text.size()) == SEDS_OK);
  const std::string epoch_string_text(epoch_text.data());
  assert(epoch_string_text.find("Timestamp: 1700000000123 (2023-11-14 22:13:20.123Z)") != std::string::npos);
  assert(epoch_string_text.find("Data: (\"hello\")") != std::string::npos);
}

void test_packet_roundtrip_gps_exact() {
  const float gps[] = {5.2141414f, 3.1342144f, 1.1231232f};
  const uint32_t endpoints[] = {SEDS_EP_SD_CARD, SEDS_EP_RADIO};
  const SedsPacketView pkt{
      .ty = SEDS_DT_GPS_DATA,
      .data_size = sizeof(gps),
      .sender = "TEST_PLATFORM",
      .sender_len = 13,
      .endpoints = endpoints,
      .num_endpoints = 2,
      .timestamp = 0,
      .payload = reinterpret_cast<const uint8_t *>(gps),
      .payload_len = sizeof(gps),
  };

  ASSERT_GT(seds_pkt_serialize_len(&pkt), 0);
  std::vector<uint8_t> wire(static_cast<size_t>(seds_pkt_serialize_len(&pkt)));
  ASSERT_EQ(seds_pkt_serialize(&pkt, wire.data(), wire.size()), static_cast<int32_t>(wire.size()));

  SedsOwnedPacket *owned = seds_pkt_deserialize_owned(wire.data(), wire.size());
  ASSERT_NE(owned, nullptr);
  SedsPacketView view{};
  ASSERT_EQ(seds_owned_pkt_view(owned, &view), SEDS_OK);
  ASSERT_EQ(view.ty, pkt.ty);
  ASSERT_EQ(view.data_size, pkt.data_size);
  ASSERT_EQ(view.timestamp, pkt.timestamp);
  ASSERT_EQ(view.num_endpoints, pkt.num_endpoints);
  ASSERT_EQ(std::memcmp(view.endpoints, pkt.endpoints, pkt.num_endpoints * sizeof(uint32_t)), 0);
  ASSERT_EQ(view.payload_len, pkt.payload_len);
  ASSERT_EQ(std::memcmp(view.payload, pkt.payload, pkt.payload_len), 0);
  seds_owned_pkt_free(owned);
}

void test_packet_hex_string_matches_expectation() {
  const float gps[] = {19.0f, 33.0f, 52.0f};
  const uint32_t endpoints[] = {SEDS_EP_SD_CARD, SEDS_EP_RADIO};
  const SedsPacketView pkt{
      .ty = SEDS_DT_GPS_DATA,
      .data_size = sizeof(gps),
      .sender = "TEST_PLATFORM",
      .sender_len = 13,
      .endpoints = endpoints,
      .num_endpoints = 2,
      .timestamp = 1123581321ull,
      .payload = reinterpret_cast<const uint8_t *>(gps),
      .payload_len = sizeof(gps),
  };
  std::vector<char> text(static_cast<size_t>(seds_pkt_to_string_len(&pkt)));
  ASSERT_EQ(seds_pkt_to_string(&pkt, text.data(), text.size()), SEDS_OK);
  ASSERT_STREQ(text.data(),
               "{Type: GPS_DATA, Data Size: 12, Sender: TEST_PLATFORM, Endpoints: [SD_CARD, RADIO], Timestamp: "
               "1123581321 (312h 06m 21s 321ms), Data: (19.00000000, 33.00000000, 52.00000000)}");
}

void test_header_string_matches_expectation() {
  const float gps[] = {1.0f, 2.0f, 3.0f};
  const uint32_t endpoints[] = {SEDS_EP_SD_CARD, SEDS_EP_RADIO};
  const SedsPacketView pkt{
      .ty = SEDS_DT_GPS_DATA,
      .data_size = sizeof(gps),
      .sender = "TEST_PLATFORM",
      .sender_len = 13,
      .endpoints = endpoints,
      .num_endpoints = 2,
      .timestamp = 0,
      .payload = reinterpret_cast<const uint8_t *>(gps),
      .payload_len = sizeof(gps),
  };
  std::vector<char> header(static_cast<size_t>(seds_pkt_header_string_len(&pkt)));
  ASSERT_EQ(seds_pkt_header_string(&pkt, header.data(), header.size()), SEDS_OK);
  ASSERT_STREQ(header.data(), "Type: GPS_DATA, Data Size: 12, Sender: TEST_PLATFORM, Endpoints: [SD_CARD, RADIO], "
                              "Timestamp: 0 (0s 000ms)");
}

void test_error_enum_code_roundtrip_and_strings() {
  constexpr int32_t codes[] = {
      SEDS_INVALID_TYPE,  SEDS_EMPTY_ENDPOINTS, SEDS_DESERIALIZE,       SEDS_IO,
      SEDS_HANDLER_ERROR, SEDS_MISSING_PAYLOAD, SEDS_TIMESTAMP_INVALID,
  };
  for (const int32_t code : codes) {
    const int32_t needed = seds_error_to_string_len(code);
    ASSERT_GT(needed, 1);
    std::vector<char> text(static_cast<size_t>(needed));
    ASSERT_EQ(seds_error_to_string(code, text.data(), text.size()), SEDS_OK);
    ASSERT_FALSE(std::string(text.data()).empty());
  }
}

void test_c_abi_topology_export_matches_topology_snapshot_shape() {
  const auto router = make_router(Seds_RM_Sink);
  ASSERT_NE(router, nullptr);
  const int32_t side = seds_router_add_side_serialized(router.get(), "A", 1, capture_tx, nullptr, false);
  ASSERT_GE(side, 0);

  const std::vector<seds::TopologyBoardNode> topology = {
      {"REMOTE_A", {SEDS_EP_SD_CARD}, {"SRC_A"}, {"SENSOR_B"}},
      {"SENSOR_B", {SEDS_EP_RADIO}, {}, {"REMOTE_A"}},
  };
  const auto topology_pkt = seds::build_discovery_topology("REMOTE_A", 0, topology);
  const auto view = topology_pkt.view();
  ASSERT_EQ(seds_router_receive_from_side(router.get(), static_cast<uint32_t>(side), &view), SEDS_OK);

  const int32_t needed = seds_router_export_topology_len(router.get());
  ASSERT_GT(needed, 1);
  std::vector<char> json(static_cast<size_t>(needed));
  ASSERT_EQ(seds_router_export_topology(router.get(), json.data(), json.size()), SEDS_OK);
  const std::string text(json.data());
  ASSERT_NE(text.find("\"routers\":["), std::string::npos);
  ASSERT_NE(text.find("\"announcers\":["), std::string::npos);
  ASSERT_NE(text.find("\"sender_id\":\"SENSOR_B\""), std::string::npos);
  ASSERT_NE(text.find("\"reachable_endpoints\":[1]"), std::string::npos);

  const auto relay = make_relay();
  ASSERT_NE(relay, nullptr);
  const int32_t relay_side = seds_relay_add_side_serialized(relay.get(), "B", 1, capture_tx, nullptr, false);
  ASSERT_GE(relay_side, 0);
  ASSERT_EQ(seds_relay_rx_packet_from_side(relay.get(), static_cast<uint32_t>(relay_side), &view), SEDS_OK);
  ASSERT_EQ(seds_relay_process_rx_queue(relay.get()), SEDS_OK);
  const int32_t relay_needed = seds_relay_export_topology_len(relay.get());
  ASSERT_GT(relay_needed, 1);
  std::vector<char> relay_json(static_cast<size_t>(relay_needed));
  ASSERT_EQ(seds_relay_export_topology(relay.get(), relay_json.data(), relay_json.size()), SEDS_OK);
  ASSERT_NE(std::string(relay_json.data()).find("\"announcers\":["), std::string::npos);
}

void test_deserialize_header_only_short_buffer_fails() {
  const uint8_t tiny[] = {0x00u};
  const uint8_t truncated[] = {0x00u, 0x80u};
  ASSERT_FALSE(seds::peek_frame_info(tiny, sizeof(tiny), false).has_value());
  ASSERT_FALSE(seds::peek_frame_info(truncated, sizeof(truncated), false).has_value());
}

void test_deserialize_header_only_then_full_parse_matches() {
  const float gps[] = {5.25f, 3.5f, 1.0f};
  const uint32_t endpoints[] = {SEDS_EP_SD_CARD, SEDS_EP_RADIO};
  const SedsPacketView pkt{
      .ty = SEDS_DT_GPS_DATA,
      .data_size = sizeof(gps),
      .sender = "TEST_PLATFORM",
      .sender_len = 13,
      .endpoints = endpoints,
      .num_endpoints = 2,
      .timestamp = 42,
      .payload = reinterpret_cast<const uint8_t *>(gps),
      .payload_len = sizeof(gps),
  };
  std::vector<uint8_t> wire(static_cast<size_t>(seds_pkt_serialize_len(&pkt)));
  ASSERT_EQ(seds_pkt_serialize(&pkt, wire.data(), wire.size()), static_cast<int32_t>(wire.size()));

  const auto env = seds::peek_frame_info(wire.data(), wire.size(), true);
  ASSERT_TRUE(env.has_value());
  ASSERT_EQ(env->envelope.ty, pkt.ty);
  ASSERT_EQ(env->envelope.sender, "TEST_PLATFORM");
  ASSERT_EQ(env->envelope.timestamp_ms, pkt.timestamp);
  ASSERT_EQ(env->envelope.endpoints, (std::vector<uint32_t>{SEDS_EP_SD_CARD, SEDS_EP_RADIO}));

  SedsOwnedPacket *owned = seds_pkt_deserialize_owned(wire.data(), wire.size());
  ASSERT_NE(owned, nullptr);
  SedsPacketView roundtrip{};
  ASSERT_EQ(seds_owned_pkt_view(owned, &roundtrip), SEDS_OK);
  ASSERT_EQ(roundtrip.ty, pkt.ty);
  ASSERT_EQ(roundtrip.payload_len, pkt.payload_len);
  ASSERT_EQ(std::memcmp(roundtrip.payload, pkt.payload, pkt.payload_len), 0);
  seds_owned_pkt_free(owned);
}

void test_packet_validate_rejects_empty_endpoints_and_size_mismatch() {
  const float gps[] = {1.0f, 2.0f, 3.0f};
  const SedsPacketView empty_endpoints{
      .ty = SEDS_DT_GPS_DATA,
      .data_size = sizeof(gps),
      .sender = "X",
      .sender_len = 1,
      .endpoints = nullptr,
      .num_endpoints = 0,
      .timestamp = 0,
      .payload = reinterpret_cast<const uint8_t *>(gps),
      .payload_len = sizeof(gps),
  };
  ASSERT_EQ(seds_pkt_serialize_len(&empty_endpoints), SEDS_BAD_ARG);

  const uint8_t bad_payload[13] = {};
  const auto router = make_router(Seds_RM_Sink);
  ASSERT_NE(router, nullptr);
  ASSERT_EQ(seds_router_log_bytes(router.get(), SEDS_DT_GPS_DATA, bad_payload, sizeof(bad_payload)),
            SEDS_SIZE_MISMATCH);
}

void test_packet_wire_size_matches_serialized_len() {
  const float gps[] = {1.0f, 2.0f, 3.0f};
  const uint32_t endpoints[] = {SEDS_EP_SD_CARD, SEDS_EP_RADIO};
  const SedsPacketView pkt{
      .ty = SEDS_DT_GPS_DATA,
      .data_size = sizeof(gps),
      .sender = "TEST",
      .sender_len = 4,
      .endpoints = endpoints,
      .num_endpoints = 2,
      .timestamp = 9,
      .payload = reinterpret_cast<const uint8_t *>(gps),
      .payload_len = sizeof(gps),
  };
  const int32_t need = seds_pkt_serialize_len(&pkt);
  ASSERT_GT(need, 0);
  std::vector<uint8_t> wire(static_cast<size_t>(need));
  ASSERT_EQ(seds_pkt_serialize(&pkt, wire.data(), wire.size()), need);
  ASSERT_EQ(static_cast<size_t>(need), wire.size());
}

void test_error_payload_is_truncated_to_meta_size() {
  ErrorCapture local{};
  Capture tx{};
  const SedsLocalEndpointDesc handlers[] = {
      {.endpoint = SEDS_EP_SD_CARD,
       .packet_handler = capture_error_packet,
       .serialized_handler = nullptr,
       .user = &local},
  };
  const auto router = make_router(Seds_RM_Sink, nullptr, nullptr, handlers, 1);
  ASSERT_NE(router, nullptr);
  ASSERT_GE(seds_router_add_side_serialized(router.get(), "BUS", 3, always_fail_tx, &tx, false), 0);

  const float gps[] = {1.0f, 2.0f, 3.0f};
  ASSERT_EQ(seds_router_log_f32(router.get(), SEDS_DT_GPS_DATA, gps, 3), SEDS_HANDLER_ERROR);
  ASSERT_EQ(local.error_hits, 1u);
  ASSERT_FALSE(local.last_error.empty());
  ASSERT_LT(local.last_error.size(), 8192u);
}

void test_deserialize_packet_rejects_overflowed_varint() {
  const uint8_t invalid_varint_wire[] = {
      0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xE7, 0x6B, 0x39, 0xC5,
  };
  ASSERT_EQ(seds_pkt_validate_serialized(invalid_varint_wire, sizeof(invalid_varint_wire)), SEDS_DESERIALIZE);
}

void test_serializer_is_canonical_roundtrip() {
  const std::string payload = "hello world";
  const uint32_t endpoints[] = {SEDS_EP_SD_CARD, SEDS_EP_RADIO};
  const SedsPacketView pkt{
      .ty = SEDS_DT_TELEMETRY_ERROR,
      .data_size = payload.size(),
      .sender = "sender",
      .sender_len = 6,
      .endpoints = endpoints,
      .num_endpoints = 2,
      .timestamp = 0,
      .payload = reinterpret_cast<const uint8_t *>(payload.data()),
      .payload_len = payload.size(),
  };
  std::vector<uint8_t> wire1(static_cast<size_t>(seds_pkt_serialize_len(&pkt)));
  ASSERT_EQ(seds_pkt_serialize(&pkt, wire1.data(), wire1.size()), static_cast<int32_t>(wire1.size()));

  const auto decoded = seds::deserialize_packet(wire1.data(), wire1.size());
  ASSERT_TRUE(decoded.has_value());
  const auto wire2 = seds::serialize_packet(*decoded);
  ASSERT_EQ(wire1, wire2);
}

void test_serializer_varint_scalars_grow_as_expected() {
  auto make_error = [](const size_t payload_len, const size_t sender_len, const uint64_t timestamp) {
    seds::PacketData pkt{SEDS_DT_TELEMETRY_ERROR, std::string(sender_len, 's'), {SEDS_EP_SD_CARD}, timestamp, {}};
    pkt.payload.assign(payload_len, static_cast<uint8_t>('a'));
    return pkt;
  };

  const auto small = make_error(10, 5, 0x7F);
  const auto medium = make_error(200, 200, 0x7F);
  const auto large = make_error(200, 200, 1ull << 40u);

  const auto small_wire = seds::serialize_packet(small);
  const auto medium_wire = seds::serialize_packet(medium);
  const auto large_wire = seds::serialize_packet(large);

  ASSERT_LT(small_wire.size(), medium_wire.size());
  ASSERT_LT(medium_wire.size(), large_wire.size());
}

void test_endpoints_bitpack_roundtrip_many_and_extremes() {
  std::vector<uint32_t> endpoints;
  endpoints.reserve(seds::kEndpointCount);
  for (uint32_t ep = 0; ep < seds::kEndpointCount; ++ep) {
    endpoints.push_back(ep);
  }
  const auto bitmap = seds::endpoint_bitmap(endpoints);
  const auto roundtrip = seds::parse_bitmap(bitmap.data(), bitmap.size());
  ASSERT_EQ(roundtrip, endpoints);

  seds::PacketData pkt{SEDS_DT_TELEMETRY_ERROR, "sender", endpoints, 123456, {}};
  pkt.payload.assign(257, 0x55u);
  const auto wire = seds::serialize_packet(pkt);
  const auto back = seds::deserialize_packet(wire.data(), wire.size());
  ASSERT_TRUE(back.has_value());
  ASSERT_EQ(back->endpoints, endpoints);
  ASSERT_EQ(back->payload, pkt.payload);
}

void test_peek_envelope_matches_full_parse_on_large_values() {
  seds::PacketData pkt{
      SEDS_DT_TELEMETRY_ERROR, std::string(10'000, 'S'), {SEDS_EP_SD_CARD, SEDS_EP_RADIO}, (1ull << 40u) + 123u, {}};
  pkt.payload.assign(4096, static_cast<uint8_t>('h'));

  const auto wire = seds::serialize_packet(pkt);
  const auto env = seds::peek_frame_info(wire.data(), wire.size(), true);
  const auto full = seds::deserialize_packet(wire.data(), wire.size());
  ASSERT_TRUE(env.has_value());
  ASSERT_TRUE(full.has_value());

  ASSERT_EQ(env->envelope.ty, pkt.ty);
  ASSERT_EQ(env->envelope.sender, pkt.sender);
  ASSERT_EQ(env->envelope.timestamp_ms, pkt.timestamp);
  ASSERT_EQ(env->envelope.endpoints, pkt.endpoints);

  ASSERT_EQ(full->ty, pkt.ty);
  ASSERT_EQ(full->timestamp, pkt.timestamp);
  ASSERT_EQ(full->endpoints, pkt.endpoints);
  ASSERT_EQ(full->payload, pkt.payload);
}

void test_serialize_packet_is_order_invariant_for_endpoints() {
  const float gps[] = {1.0f, 2.0f, 3.0f};
  const uint32_t endpoints_a[] = {SEDS_EP_RADIO, SEDS_EP_SD_CARD};
  const uint32_t endpoints_b[] = {SEDS_EP_SD_CARD, SEDS_EP_RADIO};
  const SedsPacketView pkt_a{
      .ty = SEDS_DT_GPS_DATA,
      .data_size = sizeof(gps),
      .sender = "TEST",
      .sender_len = 4,
      .endpoints = endpoints_a,
      .num_endpoints = 2,
      .timestamp = 0,
      .payload = reinterpret_cast<const uint8_t *>(gps),
      .payload_len = sizeof(gps),
  };
  const SedsPacketView pkt_b{
      .ty = SEDS_DT_GPS_DATA,
      .data_size = sizeof(gps),
      .sender = "TEST",
      .sender_len = 4,
      .endpoints = endpoints_b,
      .num_endpoints = 2,
      .timestamp = 0,
      .payload = reinterpret_cast<const uint8_t *>(gps),
      .payload_len = sizeof(gps),
  };
  std::vector<uint8_t> wire_a(static_cast<size_t>(seds_pkt_serialize_len(&pkt_a)));
  std::vector<uint8_t> wire_b(static_cast<size_t>(seds_pkt_serialize_len(&pkt_b)));
  ASSERT_EQ(seds_pkt_serialize(&pkt_a, wire_a.data(), wire_a.size()), static_cast<int32_t>(wire_a.size()));
  ASSERT_EQ(seds_pkt_serialize(&pkt_b, wire_b.data(), wire_b.size()), static_cast<int32_t>(wire_b.size()));
  ASSERT_EQ(wire_a, wire_b);
}

void test_process_all_queues_timeout_zero_handles_large_queues() {
  constexpr size_t kCount = 200;
  std::atomic<size_t> tx_count{0};
  std::atomic<size_t> rx_count{0};
  const auto tx = [](const uint8_t *, size_t, void *user) -> SedsResult {
    auto *count = static_cast<std::atomic<size_t> *>(user);
    count->fetch_add(1, std::memory_order_seq_cst);
    return SEDS_OK;
  };
  const auto handler = [](const SedsPacketView *, void *user) -> SedsResult {
    auto *count = static_cast<std::atomic<size_t> *>(user);
    count->fetch_add(1, std::memory_order_seq_cst);
    return SEDS_OK;
  };

  const SedsLocalEndpointDesc locals[] = {
      {.endpoint = SEDS_EP_SD_CARD, .packet_handler = handler, .serialized_handler = nullptr, .user = &rx_count},
  };
  const auto router = make_router(Seds_RM_Sink, nullptr, nullptr, locals, 1);
  ASSERT_NE(router, nullptr);
  ASSERT_GE(seds_router_add_side_serialized(router.get(), "tx", 2, tx, &tx_count, false), 0);

  for (size_t i = 0; i < kCount; ++i) {
    const float outbound[] = {1.0f + static_cast<float>(i) * 0.01f, 2.0f, 3.0f};
    ASSERT_EQ(seds_router_log_f32_ex(router.get(), SEDS_DT_GPS_DATA, outbound, 3, nullptr, 1), SEDS_OK);

    const float inbound[] = {9.0f + static_cast<float>(i) * 0.01f, 8.0f, 7.0f};
    const SedsPacketView pkt{
        .ty = SEDS_DT_GPS_DATA,
        .data_size = sizeof(inbound),
        .sender = "SRC",
        .sender_len = 3,
        .endpoints = &kSdCardEndpoint,
        .num_endpoints = 1,
        .timestamp = static_cast<uint64_t>(i),
        .payload = reinterpret_cast<const uint8_t *>(inbound),
        .payload_len = sizeof(inbound),
    };
    ASSERT_EQ(seds_router_rx_packet_to_queue(router.get(), &pkt), SEDS_OK);
  }

  ASSERT_EQ(seds_router_process_all_queues_with_timeout(router.get(), 0), SEDS_OK);
  ASSERT_EQ(tx_count.load(std::memory_order_seq_cst), kCount);
  ASSERT_EQ(rx_count.load(std::memory_order_seq_cst), kCount * 2);
}

void test_string_getter_trims_trailing_nuls() {
  const char payload[] = {'h', 'e', 'l', 'l', 'o', '\0', '\0'};
  const uint32_t endpoints[] = {SEDS_EP_SD_CARD};
  const SedsPacketView pkt{
      .ty = SEDS_DT_MESSAGE_DATA,
      .data_size = sizeof(payload),
      .sender = "TEST",
      .sender_len = 4,
      .endpoints = endpoints,
      .num_endpoints = 1,
      .timestamp = 0,
      .payload = reinterpret_cast<const uint8_t *>(payload),
      .payload_len = sizeof(payload),
  };
  std::vector<char> out(static_cast<size_t>(seds_pkt_get_string_len(&pkt)));
  ASSERT_EQ(seds_pkt_get_string(&pkt, out.data(), out.size()), static_cast<int32_t>(sizeof(payload)));
  ASSERT_STREQ(out.data(), "hello");
}

void test_data_as_f32_roundtrips_gps() {
  const float gps[] = {1.25f, 2.5f, 3.75f};
  const uint32_t endpoints[] = {SEDS_EP_SD_CARD};
  const SedsPacketView pkt{
      .ty = SEDS_DT_GPS_DATA,
      .data_size = sizeof(gps),
      .sender = "TEST",
      .sender_len = 4,
      .endpoints = endpoints,
      .num_endpoints = 1,
      .timestamp = 0,
      .payload = reinterpret_cast<const uint8_t *>(gps),
      .payload_len = sizeof(gps),
  };
  float out[3] = {};
  ASSERT_EQ(seds_pkt_get_f32(&pkt, out, 3), 3);
  ASSERT_FLOAT_EQ(out[0], gps[0]);
  ASSERT_FLOAT_EQ(out[1], gps[1]);
  ASSERT_FLOAT_EQ(out[2], gps[2]);
}

void test_mismatched_typed_accessor_returns_type_mismatch() {
  const float gps[] = {1.0f, 2.0f, 3.0f};
  const uint32_t endpoints[] = {SEDS_EP_SD_CARD};
  const SedsPacketView pkt{
      .ty = SEDS_DT_GPS_DATA,
      .data_size = sizeof(gps),
      .sender = "TEST",
      .sender_len = 4,
      .endpoints = endpoints,
      .num_endpoints = 1,
      .timestamp = 0,
      .payload = reinterpret_cast<const uint8_t *>(gps),
      .payload_len = sizeof(gps),
  };
  uint32_t out[3] = {};
  ASSERT_EQ(seds_pkt_get_u32(&pkt, out, 3), SEDS_TYPE_MISMATCH);
}

void test_data_as_bool_decodes_nonzero() {
  const uint8_t payload[] = {0u, 2u, 255u};
  const uint32_t endpoints[] = {SEDS_EP_SD_CARD};
  const SedsPacketView pkt{
      .ty = SEDS_DT_SYSTEM_STATUS,
      .data_size = sizeof(payload),
      .sender = "TEST",
      .sender_len = 4,
      .endpoints = endpoints,
      .num_endpoints = 1,
      .timestamp = 0,
      .payload = payload,
      .payload_len = sizeof(payload),
  };
  bool out[3] = {};
  ASSERT_EQ(seds_pkt_get_bool(&pkt, out, 3), 3);
  ASSERT_FALSE(out[0]);
  ASSERT_TRUE(out[1]);
  ASSERT_TRUE(out[2]);
}

void test_header_size_is_prefix_of_wire_image() {
  const float gps[] = {1.0f, 2.0f, 3.0f};
  const uint32_t endpoints[] = {SEDS_EP_SD_CARD, SEDS_EP_RADIO};
  const SedsPacketView pkt{
      .ty = SEDS_DT_GPS_DATA,
      .data_size = sizeof(gps),
      .sender = "TEST",
      .sender_len = 4,
      .endpoints = endpoints,
      .num_endpoints = 2,
      .timestamp = 123,
      .payload = reinterpret_cast<const uint8_t *>(gps),
      .payload_len = sizeof(gps),
  };
  std::vector<uint8_t> wire(static_cast<size_t>(seds_pkt_serialize_len(&pkt)));
  ASSERT_EQ(seds_pkt_serialize(&pkt, wire.data(), wire.size()), static_cast<int32_t>(wire.size()));
  SedsOwnedHeader *header = seds_pkt_deserialize_header_owned(wire.data(), wire.size());
  ASSERT_NE(header, nullptr);
  SedsPacketView header_view{};
  ASSERT_EQ(seds_owned_header_view(header, &header_view), SEDS_OK);
  ASSERT_EQ(header_view.payload_len, 0u);
  ASSERT_LT(header_view.sender_len, wire.size());
  seds_owned_header_free(header);
}

void test_from_f32_slice_builds_valid_packet() {
  const float values[] = {5.3f, 5.3f, 5.3f};
  const uint32_t endpoints[] = {SEDS_EP_SD_CARD};
  const SedsPacketView pkt{
      .ty = SEDS_DT_GPS_DATA,
      .data_size = sizeof(values),
      .sender = "CPP",
      .sender_len = 3,
      .endpoints = endpoints,
      .num_endpoints = 1,
      .timestamp = 12345,
      .payload = reinterpret_cast<const uint8_t *>(values),
      .payload_len = sizeof(values),
  };
  ASSERT_EQ(seds_pkt_serialize_len(&pkt) > 0, true);
  ASSERT_EQ(pkt.payload_len, 12u);
  ASSERT_EQ(pkt.timestamp, 12345u);
}

void test_from_no_data_builds_valid_packet() {
  const uint32_t endpoints[] = {SEDS_EP_SD_CARD};
  const SedsPacketView pkt{
      .ty = SEDS_DT_HEARTBEAT,
      .data_size = 0,
      .sender = "CPP",
      .sender_len = 3,
      .endpoints = endpoints,
      .num_endpoints = 1,
      .timestamp = 12345,
      .payload = nullptr,
      .payload_len = 0,
  };
  ASSERT_GT(seds_pkt_serialize_len(&pkt), 0);
  ASSERT_EQ(pkt.payload_len, 0u);
  ASSERT_EQ(pkt.timestamp, 12345u);
}

void test_serialize_validation_edges() {
  const uint32_t valid_eps[] = {SEDS_EP_SD_CARD};
  const uint32_t invalid_eps[] = {9999u};
  const uint8_t one_byte[] = {0x12};

  const SedsPacketView bad_endpoint{
      .ty = SEDS_DT_MESSAGE_DATA,
      .data_size = 1,
      .sender = "X",
      .sender_len = 1,
      .endpoints = invalid_eps,
      .num_endpoints = 1,
      .timestamp = 0,
      .payload = one_byte,
      .payload_len = 1,
  };
  assert(seds_pkt_serialize_len(&bad_endpoint) == SEDS_BAD_ARG);

  const SedsPacketView empty_endpoints{
      .ty = SEDS_DT_GPS_DATA,
      .data_size = 12,
      .sender = "X",
      .sender_len = 1,
      .endpoints = nullptr,
      .num_endpoints = 0,
      .timestamp = 0,
      .payload = reinterpret_cast<const uint8_t *>("\0\0\0\0\0\0\0\0\0\0\0\0"),
      .payload_len = 12,
  };
  assert(seds_pkt_serialize_len(&empty_endpoints) == SEDS_BAD_ARG);

  SedsRouter *const router = seds_router_new(Seds_RM_Sink, nullptr, nullptr, nullptr, 0);
  assert(router != nullptr);
  assert(seds_router_log_bytes(router, SEDS_DT_DISCOVERY_ANNOUNCE, one_byte, 1) == SEDS_SIZE_MISMATCH);

  const SedsPacketView null_payload{
      .ty = SEDS_DT_HEARTBEAT,
      .data_size = 0,
      .sender = "Z",
      .sender_len = 1,
      .endpoints = valid_eps,
      .num_endpoints = 1,
      .timestamp = 0,
      .payload = nullptr,
      .payload_len = 0,
  };
  assert(seds_pkt_serialize_len(&null_payload) > 0);

  const uint8_t invalid_varint_wire[] = {
      0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xE7, 0x6B, 0x39, 0xC5,
  };
  assert(seds_pkt_validate_serialized(invalid_varint_wire, sizeof(invalid_varint_wire)) == SEDS_DESERIALIZE);
  seds_router_free(router);
}

void test_bounded_queue_behavior() {
  Capture tx{};
  SedsRouter *const router = seds_router_new(Seds_RM_Sink, nullptr, nullptr, nullptr, 0);
  assert(router != nullptr);
  assert(seds_router_add_side_serialized(router, "BUS", 3, capture_tx, &tx, false) >= 0);

  std::string too_large(seds::kMaxQueueBytes + 128, 'X');
  assert(seds_router_log_bytes_ex(router, SEDS_DT_MESSAGE_DATA, reinterpret_cast<const uint8_t *>(too_large.data()),
                                  too_large.size(), nullptr, 1) == SEDS_PACKET_TOO_LARGE);

  std::string medium(static_cast<std::string::size_type>(12u * 1024u), 'A');
  for (int i = 0; i < 6; ++i) {
    assert(seds_router_log_bytes_ex(router, SEDS_DT_MESSAGE_DATA, reinterpret_cast<const uint8_t *>(medium.data()),
                                    medium.size(), nullptr, 1) == SEDS_OK);
  }
  assert(seds_router_process_tx_queue(router) == SEDS_OK);
  assert(!tx.frames.empty());
  assert(tx.frames.size() == 6);

  seds_router_free(router);
}

void test_discovery_self_ignore_and_per_side_advertise() {
  ManualClock clock{};
  Capture side_a{};
  Capture side_b{};
  SedsRouter *const router = seds_router_new(Seds_RM_Sink, read_clock, &clock, nullptr, 0);
  assert(router != nullptr);
  assert(seds_router_set_sender(router, "SELF_A", 6) == SEDS_OK);
  const int32_t a = seds_router_add_side_serialized(router, "A", 1, capture_tx, &side_a, false);
  const int32_t b = seds_router_add_side_serialized(router, "B", 1, capture_tx, &side_b, false);
  assert(a >= 0 && b >= 0);

  seds::PacketData from_a = seds::make_internal_packet(SEDS_DT_DISCOVERY_ANNOUNCE, 0, {});
  from_a.sender = "REMOTE_A";
  from_a.endpoints = {SEDS_EP_DISCOVERY};
  seds::append_le<uint32_t>(SEDS_EP_RADIO, from_a.payload);
  auto from_a_wire = seds::serialize_packet(from_a);
  assert(seds_router_receive_serialized_from_side(router, static_cast<uint32_t>(a), from_a_wire.data(),
                                                  from_a_wire.size()) == SEDS_OK);

  seds::PacketData from_b = seds::make_internal_packet(SEDS_DT_DISCOVERY_ANNOUNCE, 0, {});
  from_b.sender = "REMOTE_B";
  from_b.endpoints = {SEDS_EP_DISCOVERY};
  seds::append_le<uint32_t>(SEDS_EP_SD_CARD, from_b.payload);
  auto from_b_wire = seds::serialize_packet(from_b);
  assert(seds_router_receive_serialized_from_side(router, static_cast<uint32_t>(b), from_b_wire.data(),
                                                  from_b_wire.size()) == SEDS_OK);

  seds::PacketData self = seds::make_internal_packet(SEDS_DT_DISCOVERY_ANNOUNCE, 0, {});
  self.sender = "SELF_A";
  self.endpoints = {SEDS_EP_DISCOVERY};
  seds::append_le<uint32_t>(SEDS_EP_TIME_SYNC, self.payload);
  auto self_wire = seds::serialize_packet(self);
  assert(seds_router_receive_serialized_from_side(router, static_cast<uint32_t>(a), self_wire.data(),
                                                  self_wire.size()) == SEDS_OK);

  assert(seds_router_announce_discovery(router) == SEDS_OK);
  assert(seds_router_process_tx_queue(router) == SEDS_OK);

  assert(!side_a.frames.empty());
  assert(!side_b.frames.empty());

  auto pkt_a = seds::deserialize_packet(side_a.frames.front().data(), side_a.frames.front().size());
  auto pkt_b = seds::deserialize_packet(side_b.frames.front().data(), side_b.frames.front().size());
  assert(pkt_a.has_value() && pkt_b.has_value());

  std::vector<uint32_t> eps_a;
  for (size_t i = 0; i + 4 <= pkt_a->payload.size(); i += 4) {
    uint32_t ep = 0;
    std::memcpy(&ep, pkt_a->payload.data() + i, 4);
    eps_a.push_back(ep);
  }
  std::vector<uint32_t> eps_b;
  for (size_t i = 0; i + 4 <= pkt_b->payload.size(); i += 4) {
    uint32_t ep = 0;
    std::memcpy(&ep, pkt_b->payload.data() + i, 4);
    eps_b.push_back(ep);
  }

  assert(std::find(eps_a.begin(), eps_a.end(), SEDS_EP_RADIO) != eps_a.end());
  assert(std::find(eps_a.begin(), eps_a.end(), SEDS_EP_SD_CARD) != eps_a.end());
  assert(std::find(eps_a.begin(), eps_a.end(), SEDS_EP_TIME_SYNC) == eps_a.end());
  assert(std::find(eps_b.begin(), eps_b.end(), SEDS_EP_RADIO) != eps_b.end());
  assert(std::find(eps_b.begin(), eps_b.end(), SEDS_EP_SD_CARD) != eps_b.end());
  assert(std::find(eps_b.begin(), eps_b.end(), SEDS_EP_TIME_SYNC) == eps_b.end());

  seds_router_free(router);
}

void test_router_sender_identity_matches_outbound_protocol() {
  Capture tx{};
  Capture local{};
  const SedsLocalEndpointDesc handlers[] = {
      {.endpoint = SEDS_EP_SD_CARD, .packet_handler = count_packet, .serialized_handler = nullptr, .user = &local},
  };
  SedsRouter *const router = seds_router_new(Seds_RM_Sink, nullptr, nullptr, handlers, 1);
  assert(router != nullptr);
  assert(seds_router_set_sender(router, "NODE_A", 6) == SEDS_OK);
  const int32_t side = seds_router_add_side_serialized(router, "BUS", 3, capture_tx, &tx, true);
  assert(side >= 0);

  const float gps[] = {1.0f, 2.0f, 3.0f};
  assert(seds_router_log_f32(router, SEDS_DT_GPS_DATA, gps, 3) == SEDS_OK);
  assert(!tx.frames.empty());
  auto logged = seds::deserialize_packet(tx.frames.front().data(), tx.frames.front().size());
  assert(logged.has_value());
  assert(logged->sender == "NODE_A");

  tx.frames.clear();
  assert(seds_router_announce_discovery(router) == SEDS_OK);
  assert(seds_router_process_tx_queue(router) == SEDS_OK);
  assert(!tx.frames.empty());
  auto discovery = seds::deserialize_packet(tx.frames.front().data(), tx.frames.front().size());
  assert(discovery.has_value());
  assert(discovery->sender == "NODE_A");

  seds::PacketData inbound{};
  inbound.ty = SEDS_DT_GPS_DATA;
  inbound.sender = "REMOTE";
  inbound.endpoints = {SEDS_EP_SD_CARD};
  inbound.timestamp = 7;
  inbound.payload.assign(reinterpret_cast<const uint8_t *>(gps),
                         reinterpret_cast<const uint8_t *>(gps) + sizeof(gps));
  const auto inbound_wire = seds::serialize_packet_with_reliable(inbound, {.flags = 0, .seq = 1, .ack = 0});
  assert(seds_router_receive_serialized_from_side(router, static_cast<uint32_t>(side), inbound_wire.data(),
                                                  inbound_wire.size()) == SEDS_OK);
  assert(seds_router_process_tx_queue(router) == SEDS_OK);
  bool saw_e2e_ack = false;
  for (const auto &frame : tx.frames) {
    const auto pkt = seds::deserialize_packet(frame.data(), frame.size());
    assert(pkt.has_value());
    if (pkt->ty == SEDS_DT_RELIABLE_ACK && pkt->sender == "E2EACK:NODE_A") {
      saw_e2e_ack = true;
    }
  }
  assert(saw_e2e_ack);

  seds_router_free(router);
}

void test_relay_discovery_sender_is_relay() {
  Capture side_a{};
  Capture side_b{};
  SedsRelay *const relay = seds_relay_new(nullptr, nullptr);
  assert(relay != nullptr);
  const int32_t a = seds_relay_add_side_serialized(relay, "A", 1, capture_tx, &side_a, false);
  const int32_t b = seds_relay_add_side_serialized(relay, "B", 1, capture_tx, &side_b, false);
  assert(a >= 0 && b >= 0);

  seds::PacketData discovery{};
  discovery.ty = SEDS_DT_DISCOVERY_ANNOUNCE;
  discovery.sender = "NODE_A";
  discovery.endpoints = {SEDS_EP_DISCOVERY};
  discovery.timestamp = 0;
  seds::append_le<uint32_t>(SEDS_EP_RADIO, discovery.payload);
  const auto wire = seds::serialize_packet(discovery);
  assert(seds_relay_rx_serialized_from_side(relay, static_cast<uint32_t>(a), wire.data(), wire.size()) == SEDS_OK);
  assert(seds_relay_process_all_queues(relay) == SEDS_OK);
  side_a.frames.clear();
  side_b.frames.clear();

  assert(seds_relay_announce_discovery(relay) == SEDS_OK);
  assert(seds_relay_process_tx_queue(relay) == SEDS_OK);
  assert(!side_a.frames.empty());
  assert(!side_b.frames.empty());
  const auto pkt_a = seds::deserialize_packet(side_a.frames.front().data(), side_a.frames.front().size());
  const auto pkt_b = seds::deserialize_packet(side_b.frames.front().data(), side_b.frames.front().size());
  assert(pkt_a.has_value());
  assert(pkt_b.has_value());
  assert(pkt_a->sender == "RELAY");
  assert(pkt_b->sender == "RELAY");

  seds_relay_free(relay);
}

void test_discovery_routes_for_outbound_packets() {
  Capture a{};
  Capture b{};
  const auto router = make_router(Seds_RM_Sink);
  ASSERT_NE(router, nullptr);
  const int32_t side_a = seds_router_add_side_serialized(router.get(), "A", 1, capture_tx, &a, false);
  const int32_t side_b = seds_router_add_side_serialized(router.get(), "B", 1, capture_tx, &b, false);
  ASSERT_GE(side_a, 0);
  ASSERT_GE(side_b, 0);

  seds::PacketData discovery = seds::make_internal_packet(SEDS_DT_DISCOVERY_ANNOUNCE, 0, {});
  discovery.sender = "REMOTE_A";
  discovery.endpoints = {SEDS_EP_DISCOVERY};
  seds::append_le<uint32_t>(SEDS_EP_RADIO, discovery.payload);
  auto bytes = seds::serialize_packet(discovery);
  ASSERT_EQ(
      seds_router_receive_serialized_from_side(router.get(), static_cast<uint32_t>(side_a), bytes.data(), bytes.size()),
      SEDS_OK);

  const float gps[] = {1.0f, 2.0f, 3.0f};
  ASSERT_EQ(seds_router_log_bytes_ex(router.get(), SEDS_DT_GPS_DATA, reinterpret_cast<const uint8_t *>(gps),
                                     sizeof(gps), nullptr, 1),
            SEDS_OK);
  ASSERT_EQ(seds_router_process_tx_queue(router.get()), SEDS_OK);
  ASSERT_EQ(a.frames.size(), 1u);
  ASSERT_TRUE(b.frames.empty());
}

void test_queued_discovery_precedes_normal_telemetry() {
  Capture tx{};
  const auto router = make_router(Seds_RM_Sink);
  ASSERT_NE(router, nullptr);
  ASSERT_GE(seds_router_add_side_serialized(router.get(), "NET", 3, capture_tx, &tx, false), 0);

  const float gps[] = {1.0f, 2.0f, 3.0f};
  ASSERT_EQ(seds_router_log_bytes_ex(router.get(), SEDS_DT_GPS_DATA, reinterpret_cast<const uint8_t *>(gps),
                                     sizeof(gps), nullptr, 1),
            SEDS_OK);
  ASSERT_EQ(seds_router_announce_discovery(router.get()), SEDS_OK);
  ASSERT_EQ(seds_router_process_tx_queue(router.get()), SEDS_OK);

  ASSERT_EQ(tx.frames.size(), 3u);
  for (size_t i = 0; i + 1 < tx.frames.size(); ++i) {
    const auto pkt = seds::deserialize_packet(tx.frames[i].data(), tx.frames[i].size());
    ASSERT_TRUE(pkt.has_value());
    ASSERT_TRUE(seds::is_discovery_control_type(pkt->ty));
  }
  const auto last = seds::deserialize_packet(tx.frames.back().data(), tx.frames.back().size());
  ASSERT_TRUE(last.has_value());
  ASSERT_EQ(last->ty, SEDS_DT_GPS_DATA);
}

void test_relay_discovery_selective_fanout() {
  Capture a{};
  Capture b{};
  Capture c{};
  const auto relay = make_relay();
  ASSERT_NE(relay, nullptr);
  const int32_t side_a = seds_relay_add_side_serialized(relay.get(), "A", 1, capture_tx, &a, false);
  const int32_t side_b = seds_relay_add_side_serialized(relay.get(), "B", 1, capture_tx, &b, false);
  const int32_t side_c = seds_relay_add_side_serialized(relay.get(), "C", 1, capture_tx, &c, false);
  ASSERT_GE(side_a, 0);
  ASSERT_GE(side_b, 0);
  ASSERT_GE(side_c, 0);

  seds::PacketData discovery = seds::make_internal_packet(SEDS_DT_DISCOVERY_ANNOUNCE, 0, {});
  discovery.sender = "NODE_A";
  discovery.endpoints = {SEDS_EP_DISCOVERY};
  seds::append_le<uint32_t>(SEDS_EP_RADIO, discovery.payload);
  auto dbytes = seds::serialize_packet(discovery);
  ASSERT_EQ(
      seds_relay_rx_serialized_from_side(relay.get(), static_cast<uint32_t>(side_a), dbytes.data(), dbytes.size()),
      SEDS_OK);
  ASSERT_EQ(seds_relay_process_all_queues(relay.get()), SEDS_OK);
  a.frames.clear();
  b.frames.clear();

  const uint32_t endpoints[] = {SEDS_EP_RADIO};
  const float payload[] = {9.0f, 8.0f, 7.0f};
  const SedsPacketView pkt{
      .ty = SEDS_DT_GPS_DATA,
      .data_size = sizeof(payload),
      .sender = "SRC",
      .sender_len = 3,
      .endpoints = endpoints,
      .num_endpoints = 1,
      .timestamp = 2,
      .payload = reinterpret_cast<const uint8_t *>(payload),
      .payload_len = sizeof(payload),
  };
  ASSERT_EQ(seds_relay_rx_packet_from_side(relay.get(), static_cast<uint32_t>(side_c), &pkt), SEDS_OK);
  ASSERT_EQ(seds_relay_process_all_queues(relay.get()), SEDS_OK);

  ASSERT_EQ(a.frames.size(), 1u);
  ASSERT_TRUE(b.frames.empty());
}

void test_relay_basic_fan_out() {
  Capture a{};
  Capture b{};
  const auto relay = make_relay();
  ASSERT_NE(relay, nullptr);
  const int32_t in = seds_relay_add_side_serialized(relay.get(), "IN", 2, capture_tx, nullptr, false);
  const int32_t out_a = seds_relay_add_side_serialized(relay.get(), "A", 1, capture_tx, &a, false);
  const int32_t out_b = seds_relay_add_side_serialized(relay.get(), "B", 1, capture_tx, &b, false);
  ASSERT_GE(in, 0);
  ASSERT_GE(out_a, 0);
  ASSERT_GE(out_b, 0);

  const uint32_t endpoints[] = {SEDS_EP_RADIO};
  const float payload[] = {9.0f, 8.0f, 7.0f};
  const SedsPacketView pkt{
      .ty = SEDS_DT_GPS_DATA,
      .data_size = sizeof(payload),
      .sender = "SRC",
      .sender_len = 3,
      .endpoints = endpoints,
      .num_endpoints = 1,
      .timestamp = 2,
      .payload = reinterpret_cast<const uint8_t *>(payload),
      .payload_len = sizeof(payload),
  };
  ASSERT_EQ(seds_relay_rx_packet_from_side(relay.get(), static_cast<uint32_t>(in), &pkt), SEDS_OK);
  ASSERT_EQ(seds_relay_process_all_queues(relay.get()), SEDS_OK);
  ASSERT_EQ(a.frames.size(), 1u);
  ASSERT_EQ(b.frames.size(), 1u);
}

void test_relay_invalid_side_id_returns_error() {
  const auto relay = make_relay();
  ASSERT_NE(relay, nullptr);
  const uint32_t endpoints[] = {SEDS_EP_RADIO};
  const float payload[] = {1.0f, 2.0f, 3.0f};
  const SedsPacketView pkt{
      .ty = SEDS_DT_GPS_DATA,
      .data_size = sizeof(payload),
      .sender = "SRC",
      .sender_len = 3,
      .endpoints = endpoints,
      .num_endpoints = 1,
      .timestamp = 1,
      .payload = reinterpret_cast<const uint8_t *>(payload),
      .payload_len = sizeof(payload),
  };
  ASSERT_EQ(seds_relay_rx_packet_from_side(relay.get(), 0, &pkt), SEDS_INVALID_LINK_ID);
}

void test_relay_timeout_limits_work_per_call() {
  std::atomic<unsigned> tx_count{0};
  StepClockState clock{0, 10};
  const auto relay = make_relay(read_step_clock, &clock);
  ASSERT_NE(relay, nullptr);
  const int32_t src = seds_relay_add_side_serialized(relay.get(), "SRC", 3, capture_tx, nullptr, false);
  const int32_t dst = seds_relay_add_side_serialized(
      relay.get(), "DST", 3,
      [](const uint8_t *, size_t, void *user) -> SedsResult {
        auto *count = static_cast<std::atomic<unsigned> *>(user);
        count->fetch_add(1, std::memory_order_seq_cst);
        return SEDS_OK;
      },
      &tx_count, false);
  ASSERT_GE(src, 0);
  ASSERT_GE(dst, 0);

  const uint32_t endpoints[] = {SEDS_EP_RADIO};
  const float payload[] = {1.0f, 2.0f, 3.0f};
  const SedsPacketView pkt{
      .ty = SEDS_DT_GPS_DATA,
      .data_size = sizeof(payload),
      .sender = "SRC",
      .sender_len = 3,
      .endpoints = endpoints,
      .num_endpoints = 1,
      .timestamp = 1,
      .payload = reinterpret_cast<const uint8_t *>(payload),
      .payload_len = sizeof(payload),
  };
  for (int i = 0; i < 5; ++i) {
    ASSERT_EQ(seds_relay_rx_packet_from_side(relay.get(), static_cast<uint32_t>(src), &pkt), SEDS_OK);
  }
  ASSERT_EQ(seds_relay_process_all_queues_with_timeout(relay.get(), 5), SEDS_OK);
  ASSERT_EQ(tx_count.load(std::memory_order_seq_cst), 0u);
  ASSERT_EQ(seds_relay_process_all_queues_with_timeout(relay.get(), 0), SEDS_OK);
  ASSERT_EQ(tx_count.load(std::memory_order_seq_cst), 5u);
}

void test_relay_concurrent_rx_is_thread_safe() {
  constexpr size_t kThreads = 4;
  constexpr size_t kIters = 25;
  std::atomic<size_t> tx_count{0};
  const auto relay = make_relay();
  ASSERT_NE(relay, nullptr);
  const int32_t src = seds_relay_add_side_serialized(relay.get(), "SRC", 3, capture_tx, nullptr, false);
  const int32_t dst = seds_relay_add_side_serialized(
      relay.get(), "DST", 3,
      [](const uint8_t *, size_t, void *user) -> SedsResult {
        auto *count = static_cast<std::atomic<size_t> *>(user);
        count->fetch_add(1, std::memory_order_seq_cst);
        return SEDS_OK;
      },
      &tx_count, false);
  ASSERT_GE(src, 0);
  ASSERT_GE(dst, 0);
  SedsRelay *const relay_raw = relay.get();

  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (size_t tid = 0; tid < kThreads; ++tid) {
    threads.emplace_back([relay_raw, tid]() {
      for (size_t i = 0; i < kIters; ++i) {
        const uint64_t idx = static_cast<uint64_t>(tid) * static_cast<uint64_t>(kIters) + static_cast<uint64_t>(i);
        const float payload[] = {static_cast<float>(idx), 0.0f, 0.0f};
        const uint32_t endpoints[] = {SEDS_EP_RADIO};
        const SedsPacketView pkt{
            .ty = SEDS_DT_GPS_DATA,
            .data_size = sizeof(payload),
            .sender = "SRC",
            .sender_len = 3,
            .endpoints = endpoints,
            .num_endpoints = 1,
            .timestamp = idx,
            .payload = reinterpret_cast<const uint8_t *>(payload),
            .payload_len = sizeof(payload),
        };
        ASSERT_EQ(seds_relay_rx_packet_from_side(relay_raw, 0, &pkt), SEDS_OK);
      }
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }
  ASSERT_EQ(seds_relay_process_all_queues_with_timeout(relay.get(), 0), SEDS_OK);
  ASSERT_EQ(tx_count.load(std::memory_order_seq_cst), kThreads * kIters);
}

void test_reliable_wire_helpers() {
  seds::PacketData pkt;
  pkt.ty = SEDS_DT_GPS_DATA;
  pkt.sender = "SRC";
  pkt.endpoints = {SEDS_EP_RADIO};
  pkt.timestamp = 77;
  const float vals[] = {1.0f, 2.0f, 3.0f};
  pkt.payload.assign(reinterpret_cast<const uint8_t *>(vals), reinterpret_cast<const uint8_t *>(vals) + sizeof(vals));

  auto wire = seds::serialize_packet_with_reliable(pkt, seds::ReliableHeaderLite{0, 11, 5});
  auto frame = seds::peek_frame_info(wire.data(), wire.size(), true);
  assert(frame.has_value());
  assert(frame->reliable.has_value());
  assert(frame->reliable->seq == 11);
  assert(frame->reliable->ack == 5);
  assert(!frame->ack_only());

  auto off = seds::reliable_header_offset(wire.data(), wire.size());
  assert(off.has_value());
  assert(seds::rewrite_reliable_header(wire.data(), wire.size(), 0, 12, 6));
  frame = seds::peek_frame_info(wire.data(), wire.size(), true);
  assert(frame.has_value());
  assert(frame->reliable->seq == 12);
  assert(frame->reliable->ack == 6);

  auto ack = seds::serialize_reliable_ack("SRC", SEDS_DT_GPS_DATA, 88, 99);
  auto ack_frame = seds::peek_frame_info(ack.data(), ack.size(), true);
  assert(ack_frame.has_value());
  assert(ack_frame->reliable.has_value());
  assert(ack_frame->ack_only());
  assert(!seds::deserialize_packet(ack.data(), ack.size()).has_value());

  SedsOwnedHeader *header = seds_pkt_deserialize_header_owned(wire.data(), wire.size());
  assert(header != nullptr);
  SedsPacketView hv{};
  assert(seds_owned_header_view(header, &hv) == SEDS_OK);
  assert(hv.payload_len == 0);
  seds_owned_header_free(header);
}

void test_discovery_and_timesync() {
  ManualClock a_clock{};
  ManualClock b_clock{};
  Capture a_to_b{};
  Capture b_to_a{};
  SedsRouter *a = seds_router_new(Seds_RM_Sink, read_clock, &a_clock, nullptr, 0);
  SedsRouter *b = seds_router_new(Seds_RM_Sink, read_clock, &b_clock, nullptr, 0);
  assert(a != nullptr && b != nullptr);
  const int32_t a_side = seds_router_add_side_serialized(a, "AB", 2, capture_tx, &a_to_b, true);
  const int32_t b_side = seds_router_add_side_serialized(b, "BA", 2, capture_tx, &b_to_a, true);
  assert(a_side >= 0 && b_side >= 0);

  assert(seds_router_configure_timesync(a, true, 1u, 1u, 5000u, 100u, 100u) == SEDS_OK);
  assert(seds_router_configure_timesync(b, true, 0u, 100u, 5000u, 100u, 100u) == SEDS_OK);
  assert(seds_router_announce_discovery(a) == SEDS_OK);
  assert(seds_router_process_tx_queue(a) == SEDS_OK);

  for (const auto &frame : a_to_b.frames) {
    assert(seds_router_rx_serialized_packet_to_queue_from_side(b, static_cast<uint32_t>(b_side), frame.data(),
                                                               frame.size()) == SEDS_OK);
  }
  a_to_b.frames.clear();
  assert(seds_router_process_rx_queue(b) == SEDS_OK);

  for (int i = 0; i < 6; ++i) {
    a_clock.now_ms += 120;
    b_clock.now_ms += 120;
    bool did_queue = false;
    assert(seds_router_poll_timesync(a, &did_queue) == SEDS_OK);
    assert(seds_router_process_tx_queue(a) == SEDS_OK);
    for (const auto &frame : a_to_b.frames) {
      assert(seds_router_rx_serialized_packet_to_queue_from_side(b, static_cast<uint32_t>(b_side), frame.data(),
                                                                 frame.size()) == SEDS_OK);
    }
    a_to_b.frames.clear();
    assert(seds_router_process_rx_queue(b) == SEDS_OK);
  }

  uint64_t network_ms = 0;
  assert(seds_router_get_network_time_ms(b, &network_ms) == SEDS_OK);
  assert(network_ms > 0);

  seds_router_free(a);
  seds_router_free(b);
}

void test_periodic_and_discovery_poll() {
  ManualClock clock{};
  Capture tx{};
  SedsRouter *r = seds_router_new(Seds_RM_Sink, read_clock, &clock, nullptr, 0);
  assert(r);
  assert(seds_router_add_side_serialized(r, "BUS", 3, capture_tx, &tx, false) >= 0);

  bool did_queue = false;
  assert(seds_router_poll_discovery(r, &did_queue) == SEDS_OK);
  assert(did_queue);
  assert(seds_router_process_tx_queue(r) == SEDS_OK);
  assert(!tx.frames.empty());

  const size_t before = tx.frames.size();
  did_queue = true;
  clock.now_ms = 10;
  assert(seds_router_poll_discovery(r, &did_queue) == SEDS_OK);
  assert(!did_queue);
  assert(tx.frames.size() == before);

  clock.now_ms = 6000;
  assert(seds_router_periodic_no_timesync(r, 0) == SEDS_OK);
  assert(tx.frames.size() > before);

  SedsRelay *relay = seds_relay_new(read_clock, &clock);
  assert(relay);
  Capture relay_tx{};
  assert(seds_relay_add_side_serialized(relay, "BUS", 3, capture_tx, &relay_tx, false) >= 0);
  did_queue = false;
  assert(seds_relay_poll_discovery(relay, &did_queue) == SEDS_OK);
  assert(did_queue);
  assert(seds_relay_process_all_queues(relay) == SEDS_OK);
  assert(!relay_tx.frames.empty());

  seds_relay_free(relay);
  seds_router_free(r);
}

void test_timesync_failover_monotonic() {
  ManualClock clock{1000};
  SedsRouter *r = seds_router_new(Seds_RM_Sink, read_clock, &clock, nullptr, 0);
  assert(r);
  assert(seds_router_configure_timesync(r, true, 0u, 50u, 100u, 100u, 100u) == SEDS_OK);

  seds::PacketData a = seds::make_internal_packet(SEDS_DT_TIME_SYNC_ANNOUNCE, 1000, {});
  a.sender = "SRC_A";
  seds::append_le<uint64_t>(10, a.payload);
  seds::append_le<uint64_t>(1700000000000ull, a.payload);
  auto a_wire = seds::serialize_packet(a);
  assert(seds_router_receive_serialized(r, a_wire.data(), a_wire.size()) == SEDS_OK);

  seds::PacketData b = seds::make_internal_packet(SEDS_DT_TIME_SYNC_ANNOUNCE, 1000, {});
  b.sender = "SRC_B";
  seds::append_le<uint64_t>(20, b.payload);
  seds::append_le<uint64_t>(1699999990000ull, b.payload);
  auto b_wire = seds::serialize_packet(b);
  assert(seds_router_receive_serialized(r, b_wire.data(), b_wire.size()) == SEDS_OK);

  uint64_t before = 0;
  assert(seds_router_get_network_time_ms(r, &before) == SEDS_OK);
  clock.now_ms = 1200;
  uint64_t after = 0;
  assert(seds_router_get_network_time_ms(r, &after) == SEDS_OK);
  assert(after >= before);
  seds_router_free(r);
}

void test_local_timesync_setters() {
  ManualClock clock{0};
  SedsRouter *r = seds_router_new(Seds_RM_Sink, read_clock, &clock, nullptr, 0);
  assert(r);
  assert(seds_router_configure_timesync(r, true, 1u, 1u, 1000u, 100u, 100u) == SEDS_OK);
  assert(seds_router_set_local_network_date(r, 2026, 3, 21) == SEDS_OK);
  assert(seds_router_set_local_network_time_hms_millis(r, 12, 34, 56, 250) == SEDS_OK);
  uint64_t t0 = 0;
  assert(seds_router_get_network_time_ms(r, &t0) == SEDS_OK);
  assert(t0 == 1774096496250ull);
  clock.now_ms = 50;
  uint64_t t1 = 0;
  assert(seds_router_get_network_time_ms(r, &t1) == SEDS_OK);
  assert(t1 == t0 + 50);
  SedsNetworkTime nt{};
  assert(seds_router_get_network_time(r, &nt) == SEDS_OK);
  assert(nt.has_unix_time_ms);
  assert(nt.unix_time_ms == t1);
  assert(nt.has_year && nt.year == 2026);
  assert(nt.has_month && nt.month == 3);
  assert(nt.has_day && nt.day == 21);
  assert(nt.has_hour && nt.hour == 12);
  assert(nt.has_minute && nt.minute == 34);
  assert(nt.has_second && nt.second == 56);
  assert(nt.has_nanosecond && nt.nanosecond == 300000000u);
  seds_router_free(r);
}

void test_side_enable_disable_and_remove() {
  Capture a{};
  Capture b{};
  SedsRouter *r = seds_router_new(Seds_RM_Sink, nullptr, nullptr, nullptr, 0);
  assert(r);
  const int32_t side_a = seds_router_add_side_serialized(r, "A", 1, capture_tx, &a, false);
  const int32_t side_b = seds_router_add_side_serialized(r, "B", 1, capture_tx, &b, false);
  assert(side_a >= 0 && side_b >= 0);

  const float gps[] = {1.0f, 2.0f, 3.0f};
  assert(seds_router_set_side_egress_enabled(r, side_b, false) == SEDS_OK);
  assert(seds_router_log_f32(r, SEDS_DT_GPS_DATA, gps, 3) == SEDS_OK);
  assert(a.frames.size() == 1);
  assert(b.frames.empty());

  assert(seds_router_set_side_egress_enabled(r, side_b, true) == SEDS_OK);
  assert(seds_router_remove_side(r, side_a) == SEDS_OK);
  a.frames.clear();
  b.frames.clear();
  assert(seds_router_log_f32(r, SEDS_DT_GPS_DATA, gps, 3) == SEDS_OK);
  assert(a.frames.empty());
  assert(b.frames.size() == 1);
  seds_router_free(r);
}

void test_reliable_router_flow() {
  Capture a_to_b{};
  Capture b_to_a{};
  Capture b_local{};
  const SedsLocalEndpointDesc handlers[] = {
      {.endpoint = SEDS_EP_SD_CARD, .packet_handler = count_packet, .serialized_handler = nullptr, .user = &b_local},
  };
  SedsRouter *a = seds_router_new(Seds_RM_Sink, nullptr, nullptr, nullptr, 0);
  SedsRouter *b = seds_router_new(Seds_RM_Sink, nullptr, nullptr, handlers, 1);
  assert(a && b);
  const int32_t a_side = seds_router_add_side_serialized(a, "AB", 2, capture_tx, &a_to_b, true);
  const int32_t b_side = seds_router_add_side_serialized(b, "BA", 2, capture_tx, &b_to_a, true);
  assert(a_side >= 0 && b_side >= 0);

  const float gps[] = {1.0f, 2.0f, 3.0f};
  assert(seds_router_log_f32(a, SEDS_DT_GPS_DATA, gps, 3) == SEDS_OK);
  assert(!a_to_b.frames.empty());
  auto frame = seds::peek_frame_info(a_to_b.frames.front().data(), a_to_b.frames.front().size(), true);
  assert(frame && frame->reliable && frame->reliable->seq == 1);

  assert(seds_router_receive_serialized_from_side(b, static_cast<uint32_t>(b_side), a_to_b.frames.front().data(),
                                                  a_to_b.frames.front().size()) == SEDS_OK);
  assert(seds_router_process_tx_queue(b) == SEDS_OK);
  assert(b_local.packet_hits == 1);
  assert(!b_to_a.frames.empty());
  for (const auto &frame_bytes : b_to_a.frames) {
    const auto ack_pkt = seds::deserialize_packet(frame_bytes.data(), frame_bytes.size());
    assert(ack_pkt.has_value());
    assert(ack_pkt->ty == SEDS_DT_RELIABLE_ACK);
    assert(seds_router_receive_serialized_from_side(a, static_cast<uint32_t>(a_side), frame_bytes.data(),
                                                    frame_bytes.size()) == SEDS_OK);
  }
  assert(seds_router_receive_serialized_from_side(b, static_cast<uint32_t>(b_side), a_to_b.frames.front().data(),
                                                  a_to_b.frames.front().size()) == SEDS_OK);
  assert(b_local.packet_hits == 1);

  seds_router_free(a);
  seds_router_free(b);
}

void test_reliable_retransmit_timeout() {
  ManualClock clock{};
  TimedCapture tx{&clock};
  SedsRouter *a = seds_router_new(Seds_RM_Sink, read_clock, &clock, nullptr, 0);
  assert(a);
  const int32_t side = seds_router_add_side_serialized(a, "AB", 2, timed_capture_tx, &tx, true);
  assert(side >= 0);

  const float gps[] = {9.0f, 8.0f, 7.0f};
  assert(seds_router_log_f32(a, SEDS_DT_GPS_DATA, gps, 3) == SEDS_OK);
  assert(tx.frames.size() == 1);
  const auto first = tx.frames.front().second;

  clock.now_ms = 199;
  assert(seds_router_process_tx_queue(a) == SEDS_OK);
  assert(tx.frames.size() == 1);

  clock.now_ms = 250;
  assert(seds_router_process_tx_queue(a) == SEDS_OK);
  assert(tx.frames.size() == 2);
  assert(tx.frames[1].second == first);

  seds_router_free(a);
}

void test_reliable_ordered_delivery_requires_retransmit() {
  ReliableSeqCapture delivered{};
  Capture tx{};
  const SedsLocalEndpointDesc handlers[] = {
      {.endpoint = SEDS_EP_SD_CARD,
       .packet_handler = nullptr,
       .serialized_handler = capture_reliable_seq_handler,
       .user = &delivered},
  };
  const auto router = make_router(Seds_RM_Sink, nullptr, nullptr, handlers, 1);
  ASSERT_NE(router, nullptr);
  const int32_t side = seds_router_add_side_serialized(router.get(), "SRC", 3, capture_tx, &tx, true);
  ASSERT_GE(side, 0);

  const float gps1[] = {1.0f, 2.0f, 3.0f};
  const float gps2[] = {4.0f, 5.0f, 6.0f};
  seds::PacketData pkt1;
  pkt1.ty = SEDS_DT_GPS_DATA;
  pkt1.sender = "SRC";
  pkt1.endpoints = {SEDS_EP_SD_CARD};
  pkt1.timestamp = 0;
  pkt1.payload.assign(reinterpret_cast<const uint8_t *>(gps1), reinterpret_cast<const uint8_t *>(gps1) + sizeof(gps1));
  seds::PacketData pkt2 = pkt1;
  pkt2.payload.assign(reinterpret_cast<const uint8_t *>(gps2), reinterpret_cast<const uint8_t *>(gps2) + sizeof(gps2));

  const auto seq1 = seds::serialize_packet_with_reliable(pkt1, seds::ReliableHeaderLite{0, 1, 0});
  const auto seq2 = seds::serialize_packet_with_reliable(pkt2, seds::ReliableHeaderLite{0, 2, 0});

  ASSERT_EQ(
      seds_router_receive_serialized_from_side(router.get(), static_cast<uint32_t>(side), seq2.data(), seq2.size()),
      SEDS_OK);
  ASSERT_EQ(
      seds_router_receive_serialized_from_side(router.get(), static_cast<uint32_t>(side), seq1.data(), seq1.size()),
      SEDS_OK);
  ASSERT_EQ(
      seds_router_receive_serialized_from_side(router.get(), static_cast<uint32_t>(side), seq2.data(), seq2.size()),
      SEDS_OK);

  ASSERT_EQ(delivered.seqs, (std::vector<uint32_t>{1u, 2u}));
}

void test_reliable_link_recovers_from_dropped_frames() {
  constexpr uint32_t kTotal = 6;
  ManualClock clock{};
  Capture a_to_b{};
  Capture b_to_a{};
  FloatValueCapture delivered{};
  const SedsLocalEndpointDesc handlers[] = {
      {.endpoint = SEDS_EP_RADIO,
       .packet_handler = capture_first_float_handler,
       .serialized_handler = nullptr,
       .user = &delivered},
  };

  const auto router_a = make_router(Seds_RM_Sink, read_clock, &clock);
  const auto router_b = make_router(Seds_RM_Sink, read_clock, &clock, handlers, 1);
  ASSERT_NE(router_a, nullptr);
  ASSERT_NE(router_b, nullptr);

  const int32_t a_side = seds_router_add_side_serialized(router_a.get(), "A", 1, capture_tx, &a_to_b, true);
  const int32_t b_side = seds_router_add_side_serialized(router_b.get(), "B", 1, capture_tx, &b_to_a, true);
  ASSERT_GE(a_side, 0);
  ASSERT_GE(b_side, 0);

  for (uint32_t i = 0; i < kTotal; ++i) {
    const float gps[] = {static_cast<float>(i), 0.0f, 0.0f};
    ASSERT_EQ(seds_router_log_f32(router_a.get(), SEDS_DT_GPS_DATA, gps, 3), SEDS_OK);
  }

  bool dropped_data_once = false;
  bool dropped_control_once = false;
  for (size_t iter = 0; iter < 200 && delivered.values.size() < kTotal; ++iter) {
    ASSERT_EQ(seds_router_process_all_queues(router_a.get()), SEDS_OK);
    ASSERT_EQ(seds_router_process_all_queues(router_b.get()), SEDS_OK);

    auto outbound = std::move(a_to_b.frames);
    a_to_b.frames.clear();
    for (const auto &frame_bytes : outbound) {
      const auto frame = seds::peek_frame_info(frame_bytes.data(), frame_bytes.size(), true);
      ASSERT_TRUE(frame.has_value());
      if (frame->envelope.ty == SEDS_DT_GPS_DATA && frame->reliable && !frame->ack_only() &&
          frame->reliable->seq == 1 && !dropped_data_once) {
        dropped_data_once = true;
        continue;
      }
      ASSERT_EQ(seds_router_rx_serialized_packet_to_queue_from_side(router_b.get(), static_cast<uint32_t>(b_side),
                                                                    frame_bytes.data(), frame_bytes.size()),
                SEDS_OK);
    }

    auto inbound = std::move(b_to_a.frames);
    b_to_a.frames.clear();
    for (const auto &frame_bytes : inbound) {
      const auto frame = seds::peek_frame_info(frame_bytes.data(), frame_bytes.size(), true);
      ASSERT_TRUE(frame.has_value());
      if ((frame->envelope.ty == SEDS_DT_RELIABLE_ACK || frame->envelope.ty == SEDS_DT_RELIABLE_PACKET_REQUEST) &&
          !dropped_control_once) {
        const auto pkt = seds::deserialize_packet(frame_bytes.data(), frame_bytes.size());
        ASSERT_TRUE(pkt.has_value());
        ASSERT_EQ(pkt->payload.size(), sizeof(uint32_t) * 2u);
        uint32_t acked_ty = 0;
        std::memcpy(&acked_ty, pkt->payload.data(), sizeof(acked_ty));
        if (acked_ty == SEDS_DT_GPS_DATA) {
          dropped_control_once = true;
          continue;
        }
      }
      ASSERT_EQ(seds_router_rx_serialized_packet_to_queue_from_side(router_a.get(), static_cast<uint32_t>(a_side),
                                                                    frame_bytes.data(), frame_bytes.size()),
                SEDS_OK);
    }

    ASSERT_EQ(seds_router_process_all_queues(router_a.get()), SEDS_OK);
    ASSERT_EQ(seds_router_process_all_queues(router_b.get()), SEDS_OK);
    clock.now_ms += 250;
  }

  ASSERT_TRUE(dropped_data_once);
  ASSERT_TRUE(dropped_control_once);
  ASSERT_EQ(delivered.values, (std::vector<uint32_t>{0u, 1u, 2u, 3u, 4u, 5u}));
}

void test_end_to_end_reliable_ack_routes_back_without_flooding() {
  ManualClock clock{};
  FloatValueCapture delivered{};
  Capture s_to_r{};
  Capture r_to_s{};
  Capture r_to_d{};
  Capture d_to_r{};
  Capture r_to_spur{};

  const SedsLocalEndpointDesc handlers[] = {
      {.endpoint = SEDS_EP_RADIO,
       .packet_handler = capture_first_float_handler,
       .serialized_handler = nullptr,
       .user = &delivered},
  };

  const auto source = make_router(Seds_RM_Sink, read_clock, &clock);
  const auto relay = make_relay(read_clock, &clock);
  const auto dest = make_router(Seds_RM_Sink, read_clock, &clock, handlers, 1);
  ASSERT_NE(source, nullptr);
  ASSERT_NE(relay, nullptr);
  ASSERT_NE(dest, nullptr);

  const int32_t source_side = seds_router_add_side_serialized(source.get(), "relay", 5, capture_tx, &s_to_r, true);
  const int32_t relay_source_side =
      seds_relay_add_side_serialized(relay.get(), "source", 6, capture_tx, &r_to_s, true);
  const int32_t relay_dest_side =
      seds_relay_add_side_serialized(relay.get(), "dest", 4, capture_tx, &r_to_d, true);
  const int32_t dest_side = seds_router_add_side_serialized(dest.get(), "relay", 5, capture_tx, &d_to_r, true);
  ASSERT_GE(seds_relay_add_side_serialized(relay.get(), "spur", 4, capture_tx, &r_to_spur, false), 0);
  ASSERT_GE(source_side, 0);
  ASSERT_GE(relay_source_side, 0);
  ASSERT_GE(relay_dest_side, 0);
  ASSERT_GE(dest_side, 0);

  seds::PacketData discovery = seds::make_internal_packet(SEDS_DT_DISCOVERY_ANNOUNCE, 0, {});
  seds::append_le<uint32_t>(SEDS_EP_RADIO, discovery.payload);
  const auto discovery_wire = seds::serialize_packet(discovery);
  ASSERT_EQ(seds_relay_rx_serialized_from_side(relay.get(), static_cast<uint32_t>(relay_dest_side), discovery_wire.data(),
                                               discovery_wire.size()),
            SEDS_OK);
  ASSERT_EQ(seds_relay_process_all_queues(relay.get()), SEDS_OK);
  r_to_s.frames.clear();
  r_to_spur.frames.clear();

  const float gps[] = {42.0f, 0.0f, 0.0f};
  ASSERT_EQ(seds_router_log_f32(source.get(), SEDS_DT_GPS_DATA, gps, 3), SEDS_OK);

  bool dropped_end_to_end_ack = false;
  size_t forwarded_data_frames = 0;
  size_t spur_ack_frames = 0;

  for (size_t iter = 0; iter < 200; ++iter) {
    ASSERT_EQ(seds_router_process_all_queues(source.get()), SEDS_OK);
    ASSERT_EQ(seds_relay_process_all_queues(relay.get()), SEDS_OK);
    ASSERT_EQ(seds_router_process_all_queues(dest.get()), SEDS_OK);

    auto source_frames = std::move(s_to_r.frames);
    s_to_r.frames.clear();
    for (const auto &frame : source_frames) {
      const auto info = seds::peek_frame_info(frame.data(), frame.size(), true);
      ASSERT_TRUE(info.has_value());
      if (info->envelope.ty == SEDS_DT_GPS_DATA && info->reliable && !info->ack_only()) {
        forwarded_data_frames++;
      }
      ASSERT_EQ(
          seds_relay_rx_serialized_from_side(relay.get(), static_cast<uint32_t>(relay_source_side), frame.data(), frame.size()),
          SEDS_OK);
    }

    auto relay_dest_frames = std::move(r_to_d.frames);
    r_to_d.frames.clear();
    for (const auto &frame : relay_dest_frames) {
      ASSERT_EQ(seds_router_rx_serialized_packet_to_queue_from_side(dest.get(), static_cast<uint32_t>(dest_side),
                                                                    frame.data(), frame.size()),
                SEDS_OK);
    }

    auto dest_frames = std::move(d_to_r.frames);
    d_to_r.frames.clear();
    for (const auto &frame : dest_frames) {
      const auto info = seds::peek_frame_info(frame.data(), frame.size(), true);
      ASSERT_TRUE(info.has_value());
      if (info->envelope.ty == SEDS_DT_RELIABLE_ACK) {
        const auto pkt = seds::deserialize_packet(frame.data(), frame.size());
        ASSERT_TRUE(pkt.has_value());
        if (pkt->sender.rfind("E2EACK:", 0) == 0 && pkt->payload.size() == sizeof(uint64_t) &&
            !dropped_end_to_end_ack) {
          dropped_end_to_end_ack = true;
          continue;
        }
      }
      ASSERT_EQ(
          seds_relay_rx_serialized_from_side(relay.get(), static_cast<uint32_t>(relay_dest_side), frame.data(), frame.size()),
          SEDS_OK);
    }

    auto relay_source_frames = std::move(r_to_s.frames);
    r_to_s.frames.clear();
    for (const auto &frame : relay_source_frames) {
      ASSERT_EQ(seds_router_rx_serialized_packet_to_queue_from_side(source.get(), static_cast<uint32_t>(source_side),
                                                                    frame.data(), frame.size()),
                SEDS_OK);
    }

    auto spur_frames = std::move(r_to_spur.frames);
    r_to_spur.frames.clear();
    for (const auto &frame : spur_frames) {
      const auto info = seds::peek_frame_info(frame.data(), frame.size(), true);
      ASSERT_TRUE(info.has_value());
      if (info->envelope.ty == SEDS_DT_RELIABLE_ACK) {
        spur_ack_frames++;
      }
    }

    ASSERT_EQ(seds_router_process_all_queues(source.get()), SEDS_OK);
    ASSERT_EQ(seds_relay_process_all_queues(relay.get()), SEDS_OK);
    ASSERT_EQ(seds_router_process_all_queues(dest.get()), SEDS_OK);

    if (dropped_end_to_end_ack && forwarded_data_frames >= 2u && delivered.values.size() == 1u) {
      break;
    }
    clock.now_ms += 200;
  }

  ASSERT_TRUE(dropped_end_to_end_ack);
  ASSERT_GE(forwarded_data_frames, 2u);
  ASSERT_EQ(delivered.values, (std::vector<uint32_t>{42u}));
  ASSERT_EQ(spur_ack_frames, 0u);
}

void test_end_to_end_reliable_waits_for_all_discovered_holders() {
  ManualClock clock{};
  FloatValueCapture delivered_a{};
  FloatValueCapture delivered_b{};
  Capture s_to_r{};
  Capture r_to_s{};
  Capture r_to_a{};
  Capture r_to_b{};
  Capture a_to_r{};
  Capture b_to_r{};

  const SedsLocalEndpointDesc handlers_a[] = {
      {.endpoint = SEDS_EP_RADIO,
       .packet_handler = capture_first_float_handler,
       .serialized_handler = nullptr,
       .user = &delivered_a},
  };
  const SedsLocalEndpointDesc handlers_b[] = {
      {.endpoint = SEDS_EP_RADIO,
       .packet_handler = capture_first_float_handler,
       .serialized_handler = nullptr,
       .user = &delivered_b},
  };

  const auto source = make_router(Seds_RM_Sink, read_clock, &clock);
  const auto relay = make_relay(read_clock, &clock);
  const auto dest_a = make_router(Seds_RM_Sink, read_clock, &clock, handlers_a, 1);
  const auto dest_b = make_router(Seds_RM_Sink, read_clock, &clock, handlers_b, 1);
  ASSERT_NE(source, nullptr);
  ASSERT_NE(relay, nullptr);
  ASSERT_NE(dest_a, nullptr);
  ASSERT_NE(dest_b, nullptr);

  const int32_t source_side = seds_router_add_side_serialized(source.get(), "relay", 5, capture_tx, &s_to_r, true);
  const int32_t relay_source_side =
      seds_relay_add_side_serialized(relay.get(), "source", 6, capture_tx, &r_to_s, true);
  const int32_t relay_a_side = seds_relay_add_side_serialized(relay.get(), "dest_a", 6, capture_tx, &r_to_a, true);
  const int32_t relay_b_side = seds_relay_add_side_serialized(relay.get(), "dest_b", 6, capture_tx, &r_to_b, true);
  const int32_t dest_a_side = seds_router_add_side_serialized(dest_a.get(), "relay", 5, capture_tx, &a_to_r, true);
  const int32_t dest_b_side = seds_router_add_side_serialized(dest_b.get(), "relay", 5, capture_tx, &b_to_r, true);
  ASSERT_GE(source_side, 0);
  ASSERT_GE(relay_source_side, 0);
  ASSERT_GE(relay_a_side, 0);
  ASSERT_GE(relay_b_side, 0);
  ASSERT_GE(dest_a_side, 0);
  ASSERT_GE(dest_b_side, 0);
  ASSERT_EQ(seds_router_set_sender(source.get(), "SRC_NODE", 8), SEDS_OK);
  ASSERT_EQ(seds_router_set_sender(dest_a.get(), "DST_A", 5), SEDS_OK);
  ASSERT_EQ(seds_router_set_sender(dest_b.get(), "DST_B", 5), SEDS_OK);

  seds::PacketData discovery_a = seds::make_internal_packet(SEDS_DT_DISCOVERY_ANNOUNCE, 0, {});
  discovery_a.sender = "DST_A";
  seds::append_le<uint32_t>(SEDS_EP_RADIO, discovery_a.payload);
  const auto discovery_a_wire = seds::serialize_packet(discovery_a);
  ASSERT_EQ(
      seds_relay_rx_serialized_from_side(relay.get(), static_cast<uint32_t>(relay_a_side), discovery_a_wire.data(),
                                         discovery_a_wire.size()),
      SEDS_OK);

  seds::PacketData discovery_b = seds::make_internal_packet(SEDS_DT_DISCOVERY_ANNOUNCE, 0, {});
  discovery_b.sender = "DST_B";
  seds::append_le<uint32_t>(SEDS_EP_RADIO, discovery_b.payload);
  const auto discovery_b_wire = seds::serialize_packet(discovery_b);
  ASSERT_EQ(
      seds_relay_rx_serialized_from_side(relay.get(), static_cast<uint32_t>(relay_b_side), discovery_b_wire.data(),
                                         discovery_b_wire.size()),
      SEDS_OK);
  ASSERT_EQ(seds_relay_process_all_queues(relay.get()), SEDS_OK);
  r_to_s.frames.clear();

  const float gps[] = {42.0f, 0.0f, 0.0f};
  ASSERT_EQ(seds_router_log_f32(source.get(), SEDS_DT_GPS_DATA, gps, 3), SEDS_OK);

  bool dropped_b_ack = false;
  size_t forwarded_data_frames = 0;
  for (size_t iter = 0; iter < 250; ++iter) {
    ASSERT_EQ(seds_router_process_all_queues(source.get()), SEDS_OK);
    ASSERT_EQ(seds_relay_process_all_queues(relay.get()), SEDS_OK);
    ASSERT_EQ(seds_router_process_all_queues(dest_a.get()), SEDS_OK);
    ASSERT_EQ(seds_router_process_all_queues(dest_b.get()), SEDS_OK);

    auto source_frames = std::move(s_to_r.frames);
    s_to_r.frames.clear();
    for (const auto &frame : source_frames) {
      const auto info = seds::peek_frame_info(frame.data(), frame.size(), true);
      ASSERT_TRUE(info.has_value());
      if (info->envelope.ty == SEDS_DT_GPS_DATA && !info->ack_only()) {
        forwarded_data_frames++;
      }
      ASSERT_EQ(
          seds_relay_rx_serialized_from_side(relay.get(), static_cast<uint32_t>(relay_source_side), frame.data(), frame.size()),
          SEDS_OK);
    }

    auto relay_a_frames = std::move(r_to_a.frames);
    r_to_a.frames.clear();
    for (const auto &frame : relay_a_frames) {
      ASSERT_EQ(seds_router_rx_serialized_packet_to_queue_from_side(dest_a.get(), static_cast<uint32_t>(dest_a_side),
                                                                    frame.data(), frame.size()),
                SEDS_OK);
    }
    auto relay_b_frames = std::move(r_to_b.frames);
    r_to_b.frames.clear();
    for (const auto &frame : relay_b_frames) {
      ASSERT_EQ(seds_router_rx_serialized_packet_to_queue_from_side(dest_b.get(), static_cast<uint32_t>(dest_b_side),
                                                                    frame.data(), frame.size()),
                SEDS_OK);
    }

    auto a_back = std::move(a_to_r.frames);
    a_to_r.frames.clear();
    for (const auto &frame : a_back) {
      ASSERT_EQ(
          seds_relay_rx_serialized_from_side(relay.get(), static_cast<uint32_t>(relay_a_side), frame.data(), frame.size()),
          SEDS_OK);
    }
    auto b_back = std::move(b_to_r.frames);
    b_to_r.frames.clear();
    for (const auto &frame : b_back) {
      const auto pkt = seds::deserialize_packet(frame.data(), frame.size());
      ASSERT_TRUE(pkt.has_value());
      if (pkt->ty == SEDS_DT_RELIABLE_ACK && pkt->sender == "E2EACK:DST_B" && !dropped_b_ack) {
        dropped_b_ack = true;
        continue;
      }
      ASSERT_EQ(
          seds_relay_rx_serialized_from_side(relay.get(), static_cast<uint32_t>(relay_b_side), frame.data(), frame.size()),
          SEDS_OK);
    }

    auto source_back = std::move(r_to_s.frames);
    r_to_s.frames.clear();
    for (const auto &frame : source_back) {
      ASSERT_EQ(seds_router_rx_serialized_packet_to_queue_from_side(source.get(), static_cast<uint32_t>(source_side),
                                                                    frame.data(), frame.size()),
                SEDS_OK);
    }

    ASSERT_EQ(seds_router_process_all_queues(source.get()), SEDS_OK);
    ASSERT_EQ(seds_relay_process_all_queues(relay.get()), SEDS_OK);
    ASSERT_EQ(seds_router_process_all_queues(dest_a.get()), SEDS_OK);
    ASSERT_EQ(seds_router_process_all_queues(dest_b.get()), SEDS_OK);

    if (dropped_b_ack && forwarded_data_frames >= 2u && delivered_a.values.size() == 1u && delivered_b.values.size() == 1u) {
      break;
    }
    clock.now_ms += 200;
  }

  ASSERT_TRUE(dropped_b_ack);
  ASSERT_GE(forwarded_data_frames, 2u);
  ASSERT_EQ(delivered_a.values, (std::vector<uint32_t>{42u}));
  ASSERT_EQ(delivered_b.values, (std::vector<uint32_t>{42u}));
}

void test_router_clear_queues_prevents_further_processing() {
  Capture local{};
  Capture tx{};
  const SedsLocalEndpointDesc handlers[] = {
      {.endpoint = SEDS_EP_SD_CARD, .packet_handler = count_packet, .serialized_handler = nullptr, .user = &local},
  };
  const auto router = make_router(Seds_RM_Sink, nullptr, nullptr, handlers, 1);
  ASSERT_NE(router, nullptr);
  ASSERT_GE(seds_router_add_side_serialized(router.get(), "BUS", 3, capture_tx, &tx, false), 0);

  const float gps[] = {1.0f, 2.0f, 3.0f};
  ASSERT_EQ(seds_router_log_bytes_ex(router.get(), SEDS_DT_GPS_DATA, reinterpret_cast<const uint8_t *>(gps),
                                     sizeof(gps), nullptr, 1),
            SEDS_OK);

  const SedsPacketView pkt{
      .ty = SEDS_DT_GPS_DATA,
      .data_size = sizeof(gps),
      .sender = "SRC",
      .sender_len = 3,
      .endpoints = &kSdCardEndpoint,
      .num_endpoints = 1,
      .timestamp = 1,
      .payload = reinterpret_cast<const uint8_t *>(gps),
      .payload_len = sizeof(gps),
  };
  ASSERT_EQ(seds_router_rx_packet_to_queue(router.get(), &pkt), SEDS_OK);

  ASSERT_EQ(seds_router_clear_queues(router.get()), SEDS_OK);
  ASSERT_EQ(seds_router_process_all_queues(router.get()), SEDS_OK);
  ASSERT_EQ(local.packet_hits, 0u);
  ASSERT_TRUE(tx.frames.empty());
}

void test_relay_clear_queues_drops_pending_work() {
  Capture ingress{};
  Capture out{};
  const auto relay = make_relay();
  ASSERT_NE(relay, nullptr);
  const int32_t ingress_id = seds_relay_add_side_serialized(relay.get(), "IN", 2, capture_tx, &ingress, false);
  const int32_t out_id = seds_relay_add_side_serialized(relay.get(), "OUT", 3, capture_tx, &out, false);
  ASSERT_GE(ingress_id, 0);
  ASSERT_GE(out_id, 0);

  const uint32_t endpoints[] = {SEDS_EP_RADIO};
  const float payload[] = {7.0f, 8.0f, 9.0f};
  const SedsPacketView pkt{
      .ty = SEDS_DT_GPS_DATA,
      .data_size = sizeof(payload),
      .sender = "SRC",
      .sender_len = 3,
      .endpoints = endpoints,
      .num_endpoints = 1,
      .timestamp = 5,
      .payload = reinterpret_cast<const uint8_t *>(payload),
      .payload_len = sizeof(payload),
  };
  ASSERT_EQ(seds_relay_rx_packet_from_side(relay.get(), ingress_id, &pkt), SEDS_OK);
  ASSERT_EQ(seds_relay_clear_queues(relay.get()), SEDS_OK);
  ASSERT_EQ(seds_relay_process_all_queues(relay.get()), SEDS_OK);
  ASSERT_TRUE(out.frames.empty());
}

void test_router_can_disable_ingress_for_side() {
  const auto router = make_router(Seds_RM_Sink);
  ASSERT_NE(router, nullptr);
  const int32_t side = seds_router_add_side_serialized(router.get(), "BUS", 3, capture_tx, nullptr, false);
  ASSERT_GE(side, 0);
  ASSERT_EQ(seds_router_set_side_ingress_enabled(router.get(), side, false), SEDS_OK);

  const float gps[] = {1.0f, 2.0f, 3.0f};
  const SedsPacketView pkt{
      .ty = SEDS_DT_GPS_DATA,
      .data_size = sizeof(gps),
      .sender = "SRC",
      .sender_len = 3,
      .endpoints = &kSdCardEndpoint,
      .num_endpoints = 1,
      .timestamp = 1,
      .payload = reinterpret_cast<const uint8_t *>(gps),
      .payload_len = sizeof(gps),
  };
  ASSERT_EQ(seds_router_receive_from_side(router.get(), static_cast<uint32_t>(side), &pkt), SEDS_INVALID_LINK_ID);
  ASSERT_EQ(seds_router_rx_packet_to_queue_from_side(router.get(), static_cast<uint32_t>(side), &pkt),
            SEDS_INVALID_LINK_ID);
}

void test_relay_can_disable_ingress_for_side() {
  const auto relay = make_relay();
  ASSERT_NE(relay, nullptr);
  const int32_t side = seds_relay_add_side_serialized(relay.get(), "BUS", 3, capture_tx, nullptr, false);
  ASSERT_GE(side, 0);
  ASSERT_EQ(seds_relay_set_side_ingress_enabled(relay.get(), side, false), SEDS_OK);

  const uint32_t endpoints[] = {SEDS_EP_RADIO};
  const float payload[] = {1.0f, 2.0f, 3.0f};
  const SedsPacketView pkt{
      .ty = SEDS_DT_GPS_DATA,
      .data_size = sizeof(payload),
      .sender = "SRC",
      .sender_len = 3,
      .endpoints = endpoints,
      .num_endpoints = 1,
      .timestamp = 1,
      .payload = reinterpret_cast<const uint8_t *>(payload),
      .payload_len = sizeof(payload),
  };
  ASSERT_EQ(seds_relay_rx_packet_from_side(relay.get(), static_cast<uint32_t>(side), &pkt), SEDS_INVALID_LINK_ID);
}

void test_router_rx_serialized_deduplicates_identical_frames() {
  Capture local{};
  const SedsLocalEndpointDesc handlers[] = {
      {.endpoint = SEDS_EP_SD_CARD, .packet_handler = count_packet, .serialized_handler = nullptr, .user = &local},
  };
  const auto router = make_router(Seds_RM_Sink, nullptr, nullptr, handlers, 1);
  ASSERT_NE(router, nullptr);
  const int32_t side = seds_router_add_side_serialized(router.get(), "BUS", 3, capture_tx, nullptr, false);
  ASSERT_GE(side, 0);

  const float gps[] = {1.0f, 2.0f, 3.0f};
  seds::PacketData pkt;
  pkt.ty = SEDS_DT_GPS_DATA;
  pkt.sender = "REMOTE";
  pkt.endpoints = {SEDS_EP_SD_CARD};
  pkt.timestamp = 42;
  pkt.payload.assign(reinterpret_cast<const uint8_t *>(gps), reinterpret_cast<const uint8_t *>(gps) + sizeof(gps));
  const auto wire = seds::serialize_packet(pkt);

  ASSERT_EQ(
      seds_router_receive_serialized_from_side(router.get(), static_cast<uint32_t>(side), wire.data(), wire.size()),
      SEDS_OK);
  ASSERT_EQ(
      seds_router_receive_serialized_from_side(router.get(), static_cast<uint32_t>(side), wire.data(), wire.size()),
      SEDS_OK);
  ASSERT_EQ(local.packet_hits, 1u);
}

void test_router_rx_serialized_dedup_persists_across_time_advance() {
  ManualClock clock{};
  Capture local{};
  const SedsLocalEndpointDesc handlers[] = {
      {.endpoint = SEDS_EP_SD_CARD, .packet_handler = count_packet, .serialized_handler = nullptr, .user = &local},
  };
  const auto router = make_router(Seds_RM_Sink, read_clock, &clock, handlers, 1);
  ASSERT_NE(router, nullptr);
  const int32_t side = seds_router_add_side_serialized(router.get(), "BUS", 3, capture_tx, nullptr, false);
  ASSERT_GE(side, 0);

  const float gps[] = {1.0f, 2.0f, 3.0f};
  seds::PacketData pkt{SEDS_DT_GPS_DATA, "REMOTE", {SEDS_EP_SD_CARD}, 42, {}};
  pkt.payload.assign(reinterpret_cast<const uint8_t *>(gps), reinterpret_cast<const uint8_t *>(gps) + sizeof(gps));
  const auto wire = seds::serialize_packet(pkt);

  ASSERT_EQ(
      seds_router_receive_serialized_from_side(router.get(), static_cast<uint32_t>(side), wire.data(), wire.size()),
      SEDS_OK);
  clock.now_ms += 1000;
  ASSERT_EQ(
      seds_router_receive_serialized_from_side(router.get(), static_cast<uint32_t>(side), wire.data(), wire.size()),
      SEDS_OK);
  ASSERT_EQ(local.packet_hits, 1u);
}

void test_router_rx_serialized_does_not_dedupe_different_frames() {
  Capture local{};
  const SedsLocalEndpointDesc handlers[] = {
      {.endpoint = SEDS_EP_SD_CARD, .packet_handler = count_packet, .serialized_handler = nullptr, .user = &local},
  };
  const auto router = make_router(Seds_RM_Sink, nullptr, nullptr, handlers, 1);
  ASSERT_NE(router, nullptr);
  const int32_t side = seds_router_add_side_serialized(router.get(), "BUS", 3, capture_tx, nullptr, false);
  ASSERT_GE(side, 0);

  const float gps1[] = {1.0f, 2.0f, 3.0f};
  const float gps2[] = {4.0f, 5.0f, 6.0f};
  seds::PacketData pkt1{SEDS_DT_GPS_DATA, "REMOTE", {SEDS_EP_SD_CARD}, 42, {}};
  seds::PacketData pkt2{SEDS_DT_GPS_DATA, "REMOTE", {SEDS_EP_SD_CARD}, 43, {}};
  pkt1.payload.assign(reinterpret_cast<const uint8_t *>(gps1), reinterpret_cast<const uint8_t *>(gps1) + sizeof(gps1));
  pkt2.payload.assign(reinterpret_cast<const uint8_t *>(gps2), reinterpret_cast<const uint8_t *>(gps2) + sizeof(gps2));
  const auto wire1 = seds::serialize_packet(pkt1);
  const auto wire2 = seds::serialize_packet(pkt2);

  ASSERT_EQ(
      seds_router_receive_serialized_from_side(router.get(), static_cast<uint32_t>(side), wire1.data(), wire1.size()),
      SEDS_OK);
  ASSERT_EQ(
      seds_router_receive_serialized_from_side(router.get(), static_cast<uint32_t>(side), wire2.data(), wire2.size()),
      SEDS_OK);
  ASSERT_EQ(local.packet_hits, 2u);
}

void test_reliable_disabled_skips_ack() {
  Capture local{};
  Capture tx{};
  const SedsLocalEndpointDesc handlers[] = {
      {.endpoint = SEDS_EP_SD_CARD, .packet_handler = count_packet, .serialized_handler = nullptr, .user = &local},
  };
  const auto router = make_router(Seds_RM_Sink, nullptr, nullptr, handlers, 1);
  ASSERT_NE(router, nullptr);
  const int32_t side = seds_router_add_side_serialized(router.get(), "BUS", 3, capture_tx, &tx, false);
  ASSERT_GE(side, 0);

  const float gps[] = {1.0f, 2.0f, 3.0f};
  seds::PacketData pkt;
  pkt.ty = SEDS_DT_GPS_DATA;
  pkt.sender = "REMOTE";
  pkt.endpoints = {SEDS_EP_SD_CARD};
  pkt.timestamp = 9;
  pkt.payload.assign(reinterpret_cast<const uint8_t *>(gps), reinterpret_cast<const uint8_t *>(gps) + sizeof(gps));
  const auto wire = seds::serialize_packet_with_reliable(pkt, seds::ReliableHeaderLite{0, 1, 0});

  ASSERT_EQ(
      seds_router_receive_serialized_from_side(router.get(), static_cast<uint32_t>(side), wire.data(), wire.size()),
      SEDS_OK);
  ASSERT_EQ(local.packet_hits, 1u);
  ASSERT_TRUE(tx.frames.empty());
}

void test_relay_reliable_seq_advances_with_ack() {
  Capture ingress{};
  Capture egress{};
  const auto relay = make_relay();
  ASSERT_NE(relay, nullptr);
  const int32_t in_id = seds_relay_add_side_serialized(relay.get(), "IN", 2, capture_tx, &ingress, true);
  const int32_t out_id = seds_relay_add_side_serialized(relay.get(), "OUT", 3, capture_tx, &egress, true);
  ASSERT_GE(in_id, 0);
  ASSERT_GE(out_id, 0);

  const uint32_t endpoints[] = {SEDS_EP_RADIO};
  const float payload[] = {3.0f, 4.0f, 5.0f};
  const SedsPacketView pkt{
      .ty = SEDS_DT_GPS_DATA,
      .data_size = sizeof(payload),
      .sender = "SRC",
      .sender_len = 3,
      .endpoints = endpoints,
      .num_endpoints = 1,
      .timestamp = 1,
      .payload = reinterpret_cast<const uint8_t *>(payload),
      .payload_len = sizeof(payload),
  };

  ASSERT_EQ(seds_relay_rx_packet_from_side(relay.get(), in_id, &pkt), SEDS_OK);
  ASSERT_EQ(seds_relay_process_all_queues(relay.get()), SEDS_OK);
  ASSERT_EQ(egress.frames.size(), 1u);
  const auto first = seds::peek_frame_info(egress.frames.front().data(), egress.frames.front().size(), true);
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(first->reliable.has_value());
  ASSERT_EQ(first->reliable->seq, 1u);

  const auto ack = reliable_control_wire(SEDS_DT_RELIABLE_ACK, SEDS_DT_GPS_DATA, 1);
  ASSERT_EQ(seds_relay_rx_serialized_from_side(relay.get(), static_cast<uint32_t>(out_id), ack.data(), ack.size()),
            SEDS_OK);

  ASSERT_EQ(seds_relay_rx_packet_from_side(relay.get(), in_id, &pkt), SEDS_OK);
  ASSERT_EQ(seds_relay_process_all_queues(relay.get()), SEDS_OK);
  ASSERT_EQ(egress.frames.size(), 2u);
  auto second = seds::peek_frame_info(egress.frames.back().data(), egress.frames.back().size(), true);
  ASSERT_TRUE(second.has_value());
  ASSERT_TRUE(second->reliable.has_value());
  ASSERT_EQ(second->reliable->seq, 2u);
}

void test_relay_reliable_reorders_out_of_order_frames() {
  Capture src{};
  Capture dst{};
  const auto relay = make_relay();
  ASSERT_NE(relay, nullptr);
  const int32_t src_id = seds_relay_add_side_serialized(relay.get(), "SRC", 3, capture_tx, &src, true);
  const int32_t dst_id = seds_relay_add_side_serialized(relay.get(), "DST", 3, capture_tx, &dst, true);
  ASSERT_GE(src_id, 0);
  ASSERT_GE(dst_id, 0);

  const float gps1[] = {1.0f, 2.0f, 3.0f};
  const float gps2[] = {4.0f, 5.0f, 6.0f};
  seds::PacketData pkt1;
  pkt1.ty = SEDS_DT_GPS_DATA;
  pkt1.sender = "SRC";
  pkt1.endpoints = {SEDS_EP_SD_CARD};
  pkt1.timestamp = 0;
  pkt1.payload.assign(reinterpret_cast<const uint8_t *>(gps1), reinterpret_cast<const uint8_t *>(gps1) + sizeof(gps1));
  seds::PacketData pkt2 = pkt1;
  pkt2.payload.assign(reinterpret_cast<const uint8_t *>(gps2), reinterpret_cast<const uint8_t *>(gps2) + sizeof(gps2));

  const auto seq1 = seds::serialize_packet_with_reliable(pkt1, seds::ReliableHeaderLite{0, 1, 0});
  const auto seq2 = seds::serialize_packet_with_reliable(pkt2, seds::ReliableHeaderLite{0, 2, 0});

  ASSERT_EQ(seds_relay_rx_serialized_from_side(relay.get(), static_cast<uint32_t>(src_id), seq2.data(), seq2.size()),
            SEDS_OK);
  ASSERT_EQ(seds_relay_rx_serialized_from_side(relay.get(), static_cast<uint32_t>(src_id), seq1.data(), seq1.size()),
            SEDS_OK);
  ASSERT_EQ(seds_relay_rx_serialized_from_side(relay.get(), static_cast<uint32_t>(src_id), seq2.data(), seq2.size()),
            SEDS_OK);

  ASSERT_EQ(seds_relay_process_all_queues(relay.get()), SEDS_OK);
  ASSERT_EQ(dst.frames.size(), 2u);
  auto first = seds::peek_frame_info(dst.frames.front().data(), dst.frames.front().size(), true);
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(first->reliable.has_value());
  ASSERT_EQ(first->reliable->seq, 1u);

  const auto ack1 = reliable_control_wire(SEDS_DT_RELIABLE_ACK, SEDS_DT_GPS_DATA, 1);
  ASSERT_EQ(seds_relay_rx_serialized_from_side(relay.get(), static_cast<uint32_t>(dst_id), ack1.data(), ack1.size()),
            SEDS_OK);

  ASSERT_EQ(seds_relay_process_all_queues(relay.get()), SEDS_OK);
  ASSERT_EQ(dst.frames.size(), 2u);
  auto second = seds::peek_frame_info(dst.frames.back().data(), dst.frames.back().size(), true);
  ASSERT_TRUE(second.has_value());
  ASSERT_TRUE(second->reliable.has_value());
  ASSERT_EQ(second->reliable->seq, 2u);
}

void test_relay_deduplicates_identical_frames_per_side() {
  Capture out{};
  const auto relay = make_relay();
  ASSERT_NE(relay, nullptr);
  const int32_t in = seds_relay_add_side_serialized(relay.get(), "IN", 2, capture_tx, nullptr, false);
  ASSERT_GE(seds_relay_add_side_serialized(relay.get(), "OUT", 3, capture_tx, &out, false), 0);
  ASSERT_GE(in, 0);

  seds::PacketData pkt{SEDS_DT_BAROMETER_DATA, "SRC", {SEDS_EP_RADIO}, 1, {}};
  const float payload[] = {1.0f, 2.0f, 3.0f};
  pkt.payload.assign(reinterpret_cast<const uint8_t *>(payload),
                     reinterpret_cast<const uint8_t *>(payload) + sizeof(payload));
  const auto wire = seds::serialize_packet(pkt);
  ASSERT_EQ(seds_relay_rx_serialized_from_side(relay.get(), static_cast<uint32_t>(in), wire.data(), wire.size()),
            SEDS_OK);
  ASSERT_EQ(seds_relay_rx_serialized_from_side(relay.get(), static_cast<uint32_t>(in), wire.data(), wire.size()),
            SEDS_OK);
  ASSERT_EQ(seds_relay_process_all_queues(relay.get()), SEDS_OK);
  ASSERT_EQ(out.frames.size(), 1u);
}

void test_relay_dedup_persists_across_time_advance() {
  ManualClock clock{};
  Capture out{};
  const auto relay = make_relay(read_clock, &clock);
  ASSERT_NE(relay, nullptr);
  const int32_t in = seds_relay_add_side_serialized(relay.get(), "IN", 2, capture_tx, nullptr, false);
  ASSERT_GE(seds_relay_add_side_serialized(relay.get(), "OUT", 3, capture_tx, &out, false), 0);
  ASSERT_GE(in, 0);

  seds::PacketData pkt{SEDS_DT_BAROMETER_DATA, "SRC", {SEDS_EP_RADIO}, 1, {}};
  const float payload[] = {1.0f, 2.0f, 3.0f};
  pkt.payload.assign(reinterpret_cast<const uint8_t *>(payload),
                     reinterpret_cast<const uint8_t *>(payload) + sizeof(payload));
  const auto wire = seds::serialize_packet(pkt);
  ASSERT_EQ(seds_relay_rx_serialized_from_side(relay.get(), static_cast<uint32_t>(in), wire.data(), wire.size()),
            SEDS_OK);
  clock.now_ms += 1000;
  ASSERT_EQ(seds_relay_rx_serialized_from_side(relay.get(), static_cast<uint32_t>(in), wire.data(), wire.size()),
            SEDS_OK);
  ASSERT_EQ(seds_relay_process_all_queues(relay.get()), SEDS_OK);
  ASSERT_EQ(out.frames.size(), 1u);
}

void test_relay_does_not_dedupe_different_frames_from_same_side() {
  Capture out{};
  const auto relay = make_relay();
  ASSERT_NE(relay, nullptr);
  const int32_t in = seds_relay_add_side_serialized(relay.get(), "IN", 2, capture_tx, nullptr, false);
  ASSERT_GE(seds_relay_add_side_serialized(relay.get(), "OUT", 3, capture_tx, &out, false), 0);
  ASSERT_GE(in, 0);

  seds::PacketData pkt1{SEDS_DT_BAROMETER_DATA, "SRC", {SEDS_EP_RADIO}, 1, {}};
  seds::PacketData pkt2{SEDS_DT_BAROMETER_DATA, "SRC", {SEDS_EP_RADIO}, 2, {}};
  const float payload1[] = {1.0f, 2.0f, 3.0f};
  const float payload2[] = {4.0f, 5.0f, 6.0f};
  pkt1.payload.assign(reinterpret_cast<const uint8_t *>(payload1),
                      reinterpret_cast<const uint8_t *>(payload1) + sizeof(payload1));
  pkt2.payload.assign(reinterpret_cast<const uint8_t *>(payload2),
                      reinterpret_cast<const uint8_t *>(payload2) + sizeof(payload2));
  const auto wire1 = seds::serialize_packet(pkt1);
  const auto wire2 = seds::serialize_packet(pkt2);
  ASSERT_EQ(seds_relay_rx_serialized_from_side(relay.get(), static_cast<uint32_t>(in), wire1.data(), wire1.size()),
            SEDS_OK);
  ASSERT_EQ(seds_relay_rx_serialized_from_side(relay.get(), static_cast<uint32_t>(in), wire2.data(), wire2.size()),
            SEDS_OK);
  ASSERT_EQ(seds_relay_process_all_queues(relay.get()), SEDS_OK);
  ASSERT_EQ(out.frames.size(), 2u);
}

void test_reliable_sender_does_not_block_while_waiting_for_ack() {
  Capture tx{};
  const auto router = make_router(Seds_RM_Sink);
  ASSERT_NE(router, nullptr);
  const int32_t side = seds_router_add_side_serialized(router.get(), "BUS", 3, capture_tx, &tx, true);
  ASSERT_GE(side, 0);

  const float gps1[] = {1.0f, 2.0f, 3.0f};
  const float gps2[] = {4.0f, 5.0f, 6.0f};
  ASSERT_EQ(seds_router_log_f32(router.get(), SEDS_DT_GPS_DATA, gps1, 3), SEDS_OK);
  ASSERT_EQ(seds_router_log_f32(router.get(), SEDS_DT_GPS_DATA, gps2, 3), SEDS_OK);
  ASSERT_EQ(tx.frames.size(), 2u);

  const auto ack = reliable_control_wire(SEDS_DT_RELIABLE_ACK, SEDS_DT_GPS_DATA, 1);
  ASSERT_EQ(seds_router_rx_serialized_packet_to_queue_from_side(router.get(), static_cast<uint32_t>(side), ack.data(),
                                                                ack.size()),
            SEDS_OK);
  ASSERT_EQ(seds_router_process_rx_queue(router.get()), SEDS_OK);
  ASSERT_EQ(seds_router_process_tx_queue(router.get()), SEDS_OK);
  ASSERT_EQ(tx.frames.size(), 2u);
}

void test_relay_reliable_sender_does_not_block_while_waiting_for_ack() {
  Capture dst{};
  const auto relay = make_relay();
  ASSERT_NE(relay, nullptr);
  const int32_t src = seds_relay_add_side_serialized(relay.get(), "SRC", 3, capture_tx, nullptr, true);
  const int32_t out = seds_relay_add_side_serialized(relay.get(), "DST", 3, capture_tx, &dst, true);
  ASSERT_GE(src, 0);
  ASSERT_GE(out, 0);

  const uint32_t endpoints[] = {SEDS_EP_RADIO};
  const float payload1[] = {1.0f, 2.0f, 3.0f};
  const float payload2[] = {4.0f, 5.0f, 6.0f};
  const SedsPacketView pkt1{
      .ty = SEDS_DT_GPS_DATA,
      .data_size = sizeof(payload1),
      .sender = "SRC",
      .sender_len = 3,
      .endpoints = endpoints,
      .num_endpoints = 1,
      .timestamp = 1,
      .payload = reinterpret_cast<const uint8_t *>(payload1),
      .payload_len = sizeof(payload1),
  };
  const SedsPacketView pkt2{
      .ty = SEDS_DT_GPS_DATA,
      .data_size = sizeof(payload2),
      .sender = "SRC",
      .sender_len = 3,
      .endpoints = endpoints,
      .num_endpoints = 1,
      .timestamp = 2,
      .payload = reinterpret_cast<const uint8_t *>(payload2),
      .payload_len = sizeof(payload2),
  };
  ASSERT_EQ(seds_relay_rx_packet_from_side(relay.get(), static_cast<uint32_t>(src), &pkt1), SEDS_OK);
  ASSERT_EQ(seds_relay_process_all_queues(relay.get()), SEDS_OK);
  ASSERT_EQ(seds_relay_rx_packet_from_side(relay.get(), static_cast<uint32_t>(src), &pkt2), SEDS_OK);
  ASSERT_EQ(seds_relay_process_all_queues(relay.get()), SEDS_OK);
  ASSERT_EQ(dst.frames.size(), 2u);

  const auto ack = reliable_control_wire(SEDS_DT_RELIABLE_ACK, SEDS_DT_GPS_DATA, 1);
  ASSERT_EQ(seds_relay_rx_serialized_from_side(relay.get(), static_cast<uint32_t>(out), ack.data(), ack.size()),
            SEDS_OK);
  ASSERT_EQ(seds_relay_process_all_queues(relay.get()), SEDS_OK);
  ASSERT_EQ(dst.frames.size(), 2u);
}

void test_router_route_controls() {
  Capture a{};
  Capture b{};
  SedsRouter *const router = seds_router_new(Seds_RM_Sink, nullptr, nullptr, nullptr, 0);
  assert(router != nullptr);
  const int32_t side_a = seds_router_add_side_serialized(router, "A", 1, capture_tx, &a, false);
  const int32_t side_b = seds_router_add_side_serialized(router, "B", 1, capture_tx, &b, false);
  assert(side_a >= 0 && side_b >= 0);

  assert(seds_router_set_route(router, -1, side_b, false) == SEDS_OK);
  const float gps[] = {3.0f, 4.0f, 5.0f};
  assert(seds_router_log_f32(router, SEDS_DT_GPS_DATA, gps, 3) == SEDS_OK);
  assert(a.frames.size() == 1);
  assert(b.frames.empty());

  assert(seds_router_clear_route(router, -1, side_b) == SEDS_OK);
  assert(seds_router_set_source_route_mode(router, -1, Seds_RSM_Weighted) == SEDS_OK);
  assert(seds_router_set_route_weight(router, -1, side_a, 2) == SEDS_OK);
  assert(seds_router_set_route_weight(router, -1, side_b, 1) == SEDS_OK);
  a.frames.clear();
  b.frames.clear();
  for (int i = 0; i < 6; ++i) {
    assert(seds_router_log_f32(router, SEDS_DT_GPS_DATA, gps, 3) == SEDS_OK);
  }
  assert(a.frames.size() == 4);
  assert(b.frames.size() == 2);

  assert(seds_router_set_source_route_mode(router, -1, Seds_RSM_Failover) == SEDS_OK);
  assert(seds_router_set_route_priority(router, -1, side_a, 10) == SEDS_OK);
  assert(seds_router_set_route_priority(router, -1, side_b, 1) == SEDS_OK);
  a.frames.clear();
  b.frames.clear();
  assert(seds_router_log_f32(router, SEDS_DT_GPS_DATA, gps, 3) == SEDS_OK);
  assert(a.frames.empty());
  assert(b.frames.size() == 1);

  assert(seds_router_set_typed_route(router, -1, SEDS_DT_GPS_DATA, side_b, true) == SEDS_OK);
  a.frames.clear();
  b.frames.clear();
  assert(seds_router_log_f32(router, SEDS_DT_GPS_DATA, gps, 3) == SEDS_OK);
  assert(a.frames.empty());
  assert(b.frames.size() == 1);
  assert(seds_router_clear_typed_route(router, -1, SEDS_DT_GPS_DATA, side_b) == SEDS_OK);

  seds_router_free(router);
}

void test_relay_route_controls() {
  Capture ingress{};
  Capture side_a{};
  Capture side_b{};
  SedsRelay *relay = seds_relay_new(nullptr, nullptr);
  assert(relay != nullptr);
  const int32_t ingress_id = seds_relay_add_side_serialized(relay, "IN", 2, capture_tx, &ingress, false);
  const int32_t side_a_id = seds_relay_add_side_serialized(relay, "A", 1, capture_tx, &side_a, false);
  const int32_t side_b_id = seds_relay_add_side_serialized(relay, "B", 1, capture_tx, &side_b, false);
  assert(ingress_id >= 0 && side_a_id >= 0 && side_b_id >= 0);

  const uint32_t endpoints[] = {SEDS_EP_RADIO};
  const float payload[] = {1.0f, 2.0f, 3.0f};
  const SedsPacketView pkt{
      .ty = SEDS_DT_GPS_DATA,
      .data_size = sizeof(payload),
      .sender = "SRC",
      .sender_len = 3,
      .endpoints = endpoints,
      .num_endpoints = 1,
      .timestamp = 9,
      .payload = reinterpret_cast<const uint8_t *>(payload),
      .payload_len = sizeof(payload),
  };

  assert(seds_relay_set_route(relay, ingress_id, side_b_id, false) == SEDS_OK);
  assert(seds_relay_rx_packet_from_side(relay, ingress_id, &pkt) == SEDS_OK);
  assert(seds_relay_process_all_queues(relay) == SEDS_OK);
  assert(side_a.frames.size() == 1);
  assert(side_b.frames.empty());

  assert(seds_relay_clear_route(relay, ingress_id, side_b_id) == SEDS_OK);
  assert(seds_relay_set_source_route_mode(relay, ingress_id, Seds_RSM_Weighted) == SEDS_OK);
  assert(seds_relay_set_route_weight(relay, ingress_id, side_a_id, 2) == SEDS_OK);
  assert(seds_relay_set_route_weight(relay, ingress_id, side_b_id, 1) == SEDS_OK);
  side_a.frames.clear();
  side_b.frames.clear();
  for (int i = 0; i < 6; ++i) {
    assert(seds_relay_rx_packet_from_side(relay, ingress_id, &pkt) == SEDS_OK);
  }
  assert(seds_relay_process_all_queues(relay) == SEDS_OK);
  assert(side_a.frames.size() == 4);
  assert(side_b.frames.size() == 2);

  assert(seds_relay_set_typed_route(relay, ingress_id, SEDS_DT_GPS_DATA, side_b_id, true) == SEDS_OK);
  side_a.frames.clear();
  side_b.frames.clear();
  assert(seds_relay_rx_packet_from_side(relay, ingress_id, &pkt) == SEDS_OK);
  assert(seds_relay_process_all_queues(relay) == SEDS_OK);
  assert(side_a.frames.empty());
  assert(side_b.frames.size() == 1);

  assert(seds_relay_set_source_route_mode(relay, ingress_id, Seds_RSM_Failover) == SEDS_OK);
  assert(seds_relay_set_route_priority(relay, ingress_id, side_a_id, 10) == SEDS_OK);
  assert(seds_relay_set_route_priority(relay, ingress_id, side_b_id, 1) == SEDS_OK);
  assert(seds_relay_clear_typed_route(relay, ingress_id, SEDS_DT_GPS_DATA, side_b_id) == SEDS_OK);
  side_a.frames.clear();
  side_b.frames.clear();
  assert(seds_relay_rx_packet_from_side(relay, ingress_id, &pkt) == SEDS_OK);
  assert(seds_relay_process_all_queues(relay) == SEDS_OK);
  assert(side_a.frames.empty());
  assert(side_b.frames.size() == 1);

  assert(seds_relay_set_side_egress_enabled(relay, side_b_id, false) == SEDS_OK);
  side_a.frames.clear();
  side_b.frames.clear();
  assert(seds_relay_rx_packet_from_side(relay, ingress_id, &pkt) == SEDS_OK);
  assert(seds_relay_process_all_queues(relay) == SEDS_OK);
  assert(side_a.frames.size() == 1);
  assert(side_b.frames.empty());
  assert(seds_relay_remove_side(relay, side_a_id) == SEDS_OK);

  seds_relay_free(relay);
}

void test_router_typed_routes_can_target_one_or_many_sides() {
  Capture side_a{};
  Capture side_b{};
  Capture side_c{};
  Capture side_d{};
  const auto router = make_router(Seds_RM_Sink);
  ASSERT_NE(router, nullptr);
  const int32_t a = seds_router_add_side_serialized(router.get(), "A", 1, capture_tx, &side_a, false);
  const int32_t b = seds_router_add_side_serialized(router.get(), "B", 1, capture_tx, &side_b, false);
  const int32_t c = seds_router_add_side_serialized(router.get(), "C", 1, capture_tx, &side_c, false);
  const int32_t d = seds_router_add_side_serialized(router.get(), "D", 1, capture_tx, &side_d, false);
  ASSERT_GE(a, 0);
  ASSERT_GE(b, 0);
  ASSERT_GE(c, 0);
  ASSERT_GE(d, 0);

  ASSERT_EQ(seds_router_set_typed_route(router.get(), -1, SEDS_DT_GPS_DATA, b, true), SEDS_OK);
  ASSERT_EQ(seds_router_set_typed_route(router.get(), -1, SEDS_DT_GPS_DATA, d, true), SEDS_OK);

  const float gps[] = {1.0f, 2.0f, 3.0f};
  ASSERT_EQ(seds_router_log_f32(router.get(), SEDS_DT_GPS_DATA, gps, 3), SEDS_OK);
  ASSERT_TRUE(side_a.frames.empty());
  ASSERT_EQ(side_b.frames.size(), 1u);
  ASSERT_TRUE(side_c.frames.empty());
  ASSERT_EQ(side_d.frames.size(), 1u);

  ASSERT_EQ(seds_router_clear_typed_route(router.get(), -1, SEDS_DT_GPS_DATA, b), SEDS_OK);
  ASSERT_EQ(seds_router_clear_typed_route(router.get(), -1, SEDS_DT_GPS_DATA, d), SEDS_OK);
  side_a.frames.clear();
  side_b.frames.clear();
  side_c.frames.clear();
  side_d.frames.clear();

  ASSERT_EQ(seds_router_log_f32(router.get(), SEDS_DT_GPS_DATA, gps, 3), SEDS_OK);
  ASSERT_EQ(side_a.frames.size(), 1u);
  ASSERT_EQ(side_b.frames.size(), 1u);
  ASSERT_EQ(side_c.frames.size(), 1u);
  ASSERT_EQ(side_d.frames.size(), 1u);
}

void test_relay_typed_routes_can_target_one_or_many_sides() {
  Capture ingress{};
  Capture side_a{};
  Capture side_b{};
  Capture side_c{};
  Capture side_d{};
  const auto relay = make_relay();
  ASSERT_NE(relay, nullptr);
  const int32_t in = seds_relay_add_side_serialized(relay.get(), "IN", 2, capture_tx, &ingress, false);
  const int32_t a = seds_relay_add_side_serialized(relay.get(), "A", 1, capture_tx, &side_a, false);
  const int32_t b = seds_relay_add_side_serialized(relay.get(), "B", 1, capture_tx, &side_b, false);
  const int32_t c = seds_relay_add_side_serialized(relay.get(), "C", 1, capture_tx, &side_c, false);
  const int32_t d = seds_relay_add_side_serialized(relay.get(), "D", 1, capture_tx, &side_d, false);
  ASSERT_GE(in, 0);
  ASSERT_GE(a, 0);
  ASSERT_GE(b, 0);
  ASSERT_GE(c, 0);
  ASSERT_GE(d, 0);

  ASSERT_EQ(seds_relay_set_typed_route(relay.get(), in, SEDS_DT_GPS_DATA, b, true), SEDS_OK);
  ASSERT_EQ(seds_relay_set_typed_route(relay.get(), in, SEDS_DT_GPS_DATA, d, true), SEDS_OK);

  const uint32_t endpoints[] = {SEDS_EP_RADIO};
  const float payload[] = {1.0f, 2.0f, 3.0f};
  const SedsPacketView pkt{
      .ty = SEDS_DT_GPS_DATA,
      .data_size = sizeof(payload),
      .sender = "SRC",
      .sender_len = 3,
      .endpoints = endpoints,
      .num_endpoints = 1,
      .timestamp = 1,
      .payload = reinterpret_cast<const uint8_t *>(payload),
      .payload_len = sizeof(payload),
  };

  ASSERT_EQ(seds_relay_rx_packet_from_side(relay.get(), in, &pkt), SEDS_OK);
  ASSERT_EQ(seds_relay_process_all_queues(relay.get()), SEDS_OK);
  ASSERT_TRUE(side_a.frames.empty());
  ASSERT_EQ(side_b.frames.size(), 1u);
  ASSERT_TRUE(side_c.frames.empty());
  ASSERT_EQ(side_d.frames.size(), 1u);

  ASSERT_EQ(seds_relay_clear_typed_route(relay.get(), in, SEDS_DT_GPS_DATA, b), SEDS_OK);
  ASSERT_EQ(seds_relay_clear_typed_route(relay.get(), in, SEDS_DT_GPS_DATA, d), SEDS_OK);
  side_a.frames.clear();
  side_b.frames.clear();
  side_c.frames.clear();
  side_d.frames.clear();

  ASSERT_EQ(seds_relay_rx_packet_from_side(relay.get(), in, &pkt), SEDS_OK);
  ASSERT_EQ(seds_relay_process_all_queues(relay.get()), SEDS_OK);
  ASSERT_EQ(side_a.frames.size(), 1u);
  ASSERT_EQ(side_b.frames.size(), 1u);
  ASSERT_EQ(side_c.frames.size(), 1u);
  ASSERT_EQ(side_d.frames.size(), 1u);
}

void test_router_remove_side_updates_discovery_routes_and_announces_remaining_topology() {
  Capture side_a{};
  Capture side_b{};
  const auto router = make_router(Seds_RM_Sink);
  ASSERT_NE(router, nullptr);
  const int32_t a = seds_router_add_side_serialized(router.get(), "A", 1, capture_tx, &side_a, false);
  const int32_t b = seds_router_add_side_serialized(router.get(), "B", 1, capture_tx, &side_b, false);
  ASSERT_GE(a, 0);
  ASSERT_GE(b, 0);

  seds::PacketData discovery{};
  discovery.ty = SEDS_DT_DISCOVERY_ANNOUNCE;
  discovery.sender = "REMOTE_B";
  discovery.endpoints = {SEDS_EP_DISCOVERY};
  discovery.timestamp = 0;
  seds::append_le<uint32_t>(SEDS_EP_SD_CARD, discovery.payload);
  const auto bytes = seds::serialize_packet(discovery);
  ASSERT_EQ(
      seds_router_receive_serialized_from_side(router.get(), static_cast<uint32_t>(b), bytes.data(), bytes.size()),
      SEDS_OK);

  {
    std::scoped_lock lock(router->mu);
    ASSERT_EQ(router->discovery_routes.size(), 1u);
  }

  side_a.frames.clear();
  side_b.frames.clear();
  ASSERT_EQ(seds_router_remove_side(router.get(), a), SEDS_OK);
  ASSERT_EQ(seds_router_poll_discovery(router.get(), nullptr), SEDS_OK);
  ASSERT_EQ(seds_router_process_tx_queue(router.get()), SEDS_OK);

  {
    std::scoped_lock lock(router->mu);
    ASSERT_EQ(router->discovery_routes.size(), 1u);
    ASSERT_TRUE(router->discovery_routes.contains(b));
  }
  ASSERT_TRUE(side_a.frames.empty());
  ASSERT_EQ(side_b.frames.size(), 2u);
  bool saw_announce = false;
  bool saw_topology = false;
  for (const auto& frame : side_b.frames) {
    const auto pkt = seds::deserialize_packet(frame.data(), frame.size());
    ASSERT_TRUE(pkt.has_value());
    if (pkt->ty == SEDS_DT_DISCOVERY_ANNOUNCE) {
      saw_announce = true;
      ASSERT_EQ(decode_discovery_announce_payload(*pkt), (std::vector<uint32_t>{SEDS_EP_SD_CARD}));
    } else if (pkt->ty == SEDS_DT_DISCOVERY_TOPOLOGY) {
      saw_topology = true;
    }
  }
  ASSERT_TRUE(saw_announce);
  ASSERT_TRUE(saw_topology);
}

void test_relay_remove_side_updates_discovery_routes_and_announces_remaining_topology() {
  Capture side_a{};
  Capture side_b{};
  const auto relay = make_relay();
  ASSERT_NE(relay, nullptr);
  const int32_t a = seds_relay_add_side_serialized(relay.get(), "A", 1, capture_tx, &side_a, false);
  const int32_t b = seds_relay_add_side_serialized(relay.get(), "B", 1, capture_tx, &side_b, false);
  ASSERT_GE(a, 0);
  ASSERT_GE(b, 0);

  seds::PacketData discovery{};
  discovery.ty = SEDS_DT_DISCOVERY_ANNOUNCE;
  discovery.sender = "REMOTE_B";
  discovery.endpoints = {SEDS_EP_DISCOVERY};
  discovery.timestamp = 0;
  seds::append_le<uint32_t>(SEDS_EP_SD_CARD, discovery.payload);
  const auto bytes = seds::serialize_packet(discovery);
  ASSERT_EQ(seds_relay_rx_serialized_from_side(relay.get(), static_cast<uint32_t>(b), bytes.data(), bytes.size()),
            SEDS_OK);
  ASSERT_EQ(seds_relay_process_rx_queue(relay.get()), SEDS_OK);

  {
    std::scoped_lock lock(relay->mu);
    ASSERT_EQ(relay->discovery_routes.size(), 1u);
  }

  side_a.frames.clear();
  side_b.frames.clear();
  ASSERT_EQ(seds_relay_remove_side(relay.get(), a), SEDS_OK);
  bool did_queue = false;
  ASSERT_EQ(seds_relay_poll_discovery(relay.get(), &did_queue), SEDS_OK);
  ASSERT_TRUE(did_queue);
  ASSERT_EQ(seds_relay_process_tx_queue(relay.get()), SEDS_OK);

  {
    std::scoped_lock lock(relay->mu);
    ASSERT_EQ(relay->discovery_routes.size(), 1u);
    ASSERT_TRUE(relay->discovery_routes.contains(b));
  }
  ASSERT_TRUE(side_a.frames.empty());
  ASSERT_EQ(side_b.frames.size(), 2u);
  bool saw_announce = false;
  bool saw_topology = false;
  for (const auto& frame : side_b.frames) {
    const auto pkt = seds::deserialize_packet(frame.data(), frame.size());
    ASSERT_TRUE(pkt.has_value());
    if (pkt->ty == SEDS_DT_DISCOVERY_ANNOUNCE) {
      saw_announce = true;
      ASSERT_EQ(decode_discovery_announce_payload(*pkt), (std::vector<uint32_t>{SEDS_EP_SD_CARD}));
    } else if (pkt->ty == SEDS_DT_DISCOVERY_TOPOLOGY) {
      saw_topology = true;
    }
  }
  ASSERT_TRUE(saw_announce);
  ASSERT_TRUE(saw_topology);
}

void test_router_exports_board_graph_and_tracks_transitive_endpoint_holders() {
  seds::Router router;
  const int32_t side_a =
      router.add_side_serialized("A", [](std::span<const uint8_t>) { return SEDS_OK; }, false);
  ASSERT_GE(side_a, 0);

  const std::vector<seds::TopologyBoardNode> topology = {
      {"REMOTE_A", {SEDS_EP_SD_CARD}, {}, {"SENSOR_B"}},
      {"SENSOR_B", {SEDS_EP_RADIO}, {}, {"REMOTE_A"}},
  };
  const auto topology_pkt = seds::build_discovery_topology("REMOTE_A", 0, topology);
  const auto view = topology_pkt.view();
  ASSERT_EQ(seds_router_receive_from_side(router.raw(), static_cast<uint32_t>(side_a), &view), SEDS_OK);

  const auto snap = router.export_topology();
  ASSERT_EQ(snap.routes.size(), 1u);
  ASSERT_EQ(snap.routes[0].announcers.size(), 1u);
  ASSERT_EQ(snap.routes[0].announcers[0].sender_id, "REMOTE_A");
  ASSERT_TRUE(std::ranges::any_of(snap.routes[0].announcers[0].routers, [](const auto& board) {
    return board.sender_id == "SENSOR_B" && board.reachable_endpoints == std::vector<uint32_t>{SEDS_EP_RADIO};
  }));
  ASSERT_TRUE(std::ranges::any_of(snap.routers, [](const auto& board) {
    return board.sender_id == "SENSOR_B" &&
           std::ranges::find(board.connections, std::string("REMOTE_A")) != board.connections.end();
  }));
  ASSERT_TRUE(std::ranges::find(snap.advertised_endpoints, SEDS_EP_RADIO) != snap.advertised_endpoints.end());
}

void test_router_failover_route_mode_switches_when_preferred_path_expires() {
  ManualClock clock{};
  Capture side_a{};
  Capture side_b{};
  const auto router = make_router(Seds_RM_Sink, read_clock, &clock);
  ASSERT_NE(router, nullptr);
  const int32_t a = seds_router_add_side_serialized(router.get(), "A", 1, capture_tx, &side_a, false);
  const int32_t b = seds_router_add_side_serialized(router.get(), "B", 1, capture_tx, &side_b, false);
  ASSERT_GE(a, 0);
  ASSERT_GE(b, 0);

  seds::PacketData discovery_a{};
  discovery_a.ty = SEDS_DT_DISCOVERY_ANNOUNCE;
  discovery_a.sender = "REMOTE_A";
  discovery_a.endpoints = {SEDS_EP_DISCOVERY};
  discovery_a.timestamp = 0;
  seds::append_le<uint32_t>(SEDS_EP_RADIO, discovery_a.payload);
  const auto bytes_a = seds::serialize_packet(discovery_a);
  ASSERT_EQ(
      seds_router_receive_serialized_from_side(router.get(), static_cast<uint32_t>(a), bytes_a.data(), bytes_a.size()),
      SEDS_OK);

  clock.now_ms = seds::kDiscoveryTtlMs / 2;
  auto discovery_b = discovery_a;
  discovery_b.sender = "REMOTE_B";
  const auto bytes_b = seds::serialize_packet(discovery_b);
  ASSERT_EQ(
      seds_router_receive_serialized_from_side(router.get(), static_cast<uint32_t>(b), bytes_b.data(), bytes_b.size()),
      SEDS_OK);

  side_a.frames.clear();
  side_b.frames.clear();
  ASSERT_EQ(seds_router_set_source_route_mode(router.get(), -1, Seds_RSM_Failover), SEDS_OK);
  ASSERT_EQ(seds_router_set_route_priority(router.get(), -1, a, 0), SEDS_OK);
  ASSERT_EQ(seds_router_set_route_priority(router.get(), -1, b, 1), SEDS_OK);

  const float gps1[] = {1.0f, 2.0f, 3.0f};
  ASSERT_EQ(seds_router_log_f32(router.get(), SEDS_DT_GPS_DATA, gps1, 3), SEDS_OK);
  ASSERT_EQ(side_a.frames.size(), 1u);
  ASSERT_TRUE(side_b.frames.empty());

  clock.now_ms = seds::kDiscoveryTtlMs + 1;
  const float gps2[] = {2.0f, 3.0f, 4.0f};
  ASSERT_EQ(seds_router_log_f32(router.get(), SEDS_DT_GPS_DATA, gps2, 3), SEDS_OK);
  ASSERT_EQ(side_a.frames.size(), 1u);
  ASSERT_EQ(side_b.frames.size(), 1u);
}

void test_relay_failover_route_mode_switches_when_preferred_path_expires() {
  ManualClock clock{};
  Capture side_a{};
  Capture side_b{};
  Capture side_c{};
  const auto relay = make_relay(read_clock, &clock);
  ASSERT_NE(relay, nullptr);
  const int32_t a = seds_relay_add_side_serialized(relay.get(), "A", 1, capture_tx, &side_a, false);
  const int32_t b = seds_relay_add_side_serialized(relay.get(), "B", 1, capture_tx, &side_b, false);
  const int32_t c = seds_relay_add_side_serialized(relay.get(), "C", 1, capture_tx, &side_c, false);
  ASSERT_GE(a, 0);
  ASSERT_GE(b, 0);
  ASSERT_GE(c, 0);

  seds::PacketData discovery{};
  discovery.ty = SEDS_DT_DISCOVERY_ANNOUNCE;
  discovery.sender = "REMOTE_A";
  discovery.endpoints = {SEDS_EP_DISCOVERY};
  discovery.timestamp = 0;
  seds::append_le<uint32_t>(SEDS_EP_RADIO, discovery.payload);
  auto bytes_a = seds::serialize_packet(discovery);
  ASSERT_EQ(seds_relay_rx_serialized_from_side(relay.get(), static_cast<uint32_t>(a), bytes_a.data(), bytes_a.size()),
            SEDS_OK);
  ASSERT_EQ(seds_relay_process_all_queues(relay.get()), SEDS_OK);

  clock.now_ms = seds::kDiscoveryTtlMs / 2;
  discovery.sender = "REMOTE_B";
  auto bytes_b = seds::serialize_packet(discovery);
  ASSERT_EQ(seds_relay_rx_serialized_from_side(relay.get(), static_cast<uint32_t>(b), bytes_b.data(), bytes_b.size()),
            SEDS_OK);
  ASSERT_EQ(seds_relay_process_all_queues(relay.get()), SEDS_OK);

  side_a.frames.clear();
  side_b.frames.clear();
  ASSERT_EQ(seds_relay_set_source_route_mode(relay.get(), c, Seds_RSM_Failover), SEDS_OK);
  ASSERT_EQ(seds_relay_set_route_priority(relay.get(), c, a, 0), SEDS_OK);
  ASSERT_EQ(seds_relay_set_route_priority(relay.get(), c, b, 1), SEDS_OK);

  const uint32_t endpoints[] = {SEDS_EP_RADIO};
  const float payload1[] = {1.0f, 2.0f, 3.0f};
  const SedsPacketView pkt1{
      .ty = SEDS_DT_GPS_DATA,
      .data_size = sizeof(payload1),
      .sender = "SRC",
      .sender_len = 3,
      .endpoints = endpoints,
      .num_endpoints = 1,
      .timestamp = 1,
      .payload = reinterpret_cast<const uint8_t *>(payload1),
      .payload_len = sizeof(payload1),
  };
  ASSERT_EQ(seds_relay_rx_packet_from_side(relay.get(), c, &pkt1), SEDS_OK);
  ASSERT_EQ(seds_relay_process_all_queues(relay.get()), SEDS_OK);
  ASSERT_EQ(side_a.frames.size(), 1u);
  ASSERT_TRUE(side_b.frames.empty());

  clock.now_ms = seds::kDiscoveryTtlMs + 1;
  const float payload2[] = {2.0f, 3.0f, 4.0f};
  const SedsPacketView pkt2{
      .ty = SEDS_DT_GPS_DATA,
      .data_size = sizeof(payload2),
      .sender = "SRC",
      .sender_len = 3,
      .endpoints = endpoints,
      .num_endpoints = 1,
      .timestamp = 2,
      .payload = reinterpret_cast<const uint8_t *>(payload2),
      .payload_len = sizeof(payload2),
  };
  ASSERT_EQ(seds_relay_rx_packet_from_side(relay.get(), c, &pkt2), SEDS_OK);
  ASSERT_EQ(seds_relay_process_all_queues(relay.get()), SEDS_OK);
  ASSERT_EQ(side_a.frames.size(), 1u);
  ASSERT_EQ(side_b.frames.size(), 1u);
}

std::vector<std::string> decode_discovery_timesync_sources_payload(const seds::PacketData &pkt) {
  std::vector<std::string> out;
  if (pkt.payload.size() < sizeof(uint32_t)) {
    return out;
  }
  uint32_t count = 0;
  std::memcpy(&count, pkt.payload.data(), sizeof(count));
  size_t off = sizeof(count);
  for (uint32_t i = 0; i < count && off + sizeof(uint32_t) <= pkt.payload.size(); ++i) {
    uint32_t len = 0;
    std::memcpy(&len, pkt.payload.data() + off, sizeof(len));
    off += sizeof(len);
    if (off + len > pkt.payload.size()) {
      break;
    }
    out.emplace_back(reinterpret_cast<const char *>(pkt.payload.data() + off), len);
    off += len;
  }
  return out;
}

std::vector<uint32_t> decode_discovery_announce_payload(const seds::PacketData &pkt) {
  std::vector<uint32_t> out;
  for (size_t off = 0; off + sizeof(uint32_t) <= pkt.payload.size(); off += sizeof(uint32_t)) {
    uint32_t endpoint = 0;
    std::memcpy(&endpoint, pkt.payload.data() + off, sizeof(endpoint));
    out.push_back(endpoint);
  }
  std::ranges::sort(out);
  out.erase(std::ranges::unique(out).begin(), out.end());
  return out;
}

void test_periodic_no_timesync_skips_timesync_but_keeps_discovery() {
  ManualClock clock{};
  Capture tx{};
  const auto router = make_router(Seds_RM_Sink, read_clock, &clock);
  ASSERT_NE(router, nullptr);
  ASSERT_GE(seds_router_add_side_serialized(router.get(), "BUS", 3, capture_tx, &tx, false), 0);
  ASSERT_EQ(seds_router_configure_timesync(router.get(), true, 1u, 1u, 100u, 0u, 100u), SEDS_OK);

  ASSERT_EQ(seds_router_periodic_no_timesync(router.get(), 0), SEDS_OK);
  ASSERT_FALSE(tx.frames.empty());

  bool saw_discovery = false;
  bool saw_timesync = false;
  for (const auto &frame : tx.frames) {
    auto pkt = seds::deserialize_packet(frame.data(), frame.size());
    ASSERT_TRUE(pkt.has_value());
    saw_discovery = saw_discovery || pkt->ty == SEDS_DT_DISCOVERY_ANNOUNCE;
    saw_timesync = saw_timesync || pkt->ty == SEDS_DT_TIME_SYNC_ANNOUNCE;
  }
  ASSERT_TRUE(saw_discovery);
  ASSERT_FALSE(saw_timesync);
}

void test_queued_timesync_precedes_normal_telemetry() {
  ManualClock clock{};
  Capture tx{};
  const auto router = make_router(Seds_RM_Sink, read_clock, &clock);
  ASSERT_NE(router, nullptr);
  ASSERT_GE(seds_router_add_side_serialized(router.get(), "BUS", 3, capture_tx, &tx, false), 0);
  ASSERT_EQ(seds_router_configure_timesync(router.get(), true, 1u, 1u, 1000u, 0u, 100u), SEDS_OK);

  const float gps[] = {1.0f, 2.0f, 3.0f};
  ASSERT_EQ(seds_router_log_bytes_ex(router.get(), SEDS_DT_GPS_DATA, reinterpret_cast<const uint8_t *>(gps),
                                     sizeof(gps), nullptr, 1),
            SEDS_OK);
  bool did_queue = false;
  ASSERT_EQ(seds_router_poll_timesync(router.get(), &did_queue), SEDS_OK);
  ASSERT_TRUE(did_queue);
  ASSERT_EQ(seds_router_process_tx_queue(router.get()), SEDS_OK);

  ASSERT_GE(tx.frames.size(), 2u);
  const auto first = seds::deserialize_packet(tx.frames[0].data(), tx.frames[0].size());
  auto second = seds::deserialize_packet(tx.frames[1].data(), tx.frames[1].size());
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  ASSERT_EQ(first->ty, SEDS_DT_TIME_SYNC_ANNOUNCE);
  ASSERT_EQ(second->ty, SEDS_DT_GPS_DATA);
}

void test_discovery_advertises_timesync_endpoint_and_sources() {
  ManualClock clock{};
  Capture tx{};
  const auto router = make_router(Seds_RM_Sink, read_clock, &clock);
  ASSERT_NE(router, nullptr);
  ASSERT_GE(seds_router_add_side_serialized(router.get(), "BUS", 3, capture_tx, &tx, false), 0);
  ASSERT_EQ(seds_router_configure_timesync(router.get(), true, 1u, 7u, 1000u, 100u, 100u), SEDS_OK);
  ASSERT_EQ(seds_router_set_local_network_datetime_millis(router.get(), 2026, 3, 21, 12, 34, 56, 250), SEDS_OK);

  ASSERT_EQ(seds_router_announce_discovery(router.get()), SEDS_OK);
  ASSERT_EQ(seds_router_process_tx_queue(router.get()), SEDS_OK);
  ASSERT_GE(tx.frames.size(), 2u);

  bool saw_endpoint_announce = false;
  bool saw_sources_announce = false;
  for (const auto &frame : tx.frames) {
    auto pkt = seds::deserialize_packet(frame.data(), frame.size());
    ASSERT_TRUE(pkt.has_value());
    if (pkt->ty == SEDS_DT_DISCOVERY_ANNOUNCE) {
      saw_endpoint_announce = true;
      std::vector<uint32_t> endpoints;
      for (size_t i = 0; i + sizeof(uint32_t) <= pkt->payload.size(); i += sizeof(uint32_t)) {
        uint32_t endpoint = 0;
        std::memcpy(&endpoint, pkt->payload.data() + i, sizeof(endpoint));
        endpoints.push_back(endpoint);
      }
      ASSERT_TRUE(std::find(endpoints.begin(), endpoints.end(), SEDS_EP_TIME_SYNC) != endpoints.end());
    } else if (pkt->ty == SEDS_DT_DISCOVERY_TIMESYNC_SOURCES) {
      saw_sources_announce = true;
      const auto sources = decode_discovery_timesync_sources_payload(*pkt);
      ASSERT_TRUE(std::find(sources.begin(), sources.end(), "local") != sources.end());
    }
  }
  ASSERT_TRUE(saw_endpoint_announce);
  ASSERT_TRUE(saw_sources_announce);
}

void test_timesync_packets_use_discovery_candidates_and_exact_source_route() {
  ManualClock clock{};
  Capture side_a{};
  Capture side_b{};
  auto source_router = make_router(Seds_RM_Sink, read_clock, &clock);
  ASSERT_NE(source_router, nullptr);
  const int32_t a = seds_router_add_side_serialized(source_router.get(), "A", 1, capture_tx, &side_a, false);
  const int32_t b = seds_router_add_side_serialized(source_router.get(), "B", 1, capture_tx, &side_b, false);
  ASSERT_GE(a, 0);
  ASSERT_GE(b, 0);
  ASSERT_EQ(seds_router_configure_timesync(source_router.get(), true, 1u, 50u, 5000u, 100u, 100u), SEDS_OK);

  seds::PacketData discovery_a = seds::make_internal_packet(SEDS_DT_DISCOVERY_ANNOUNCE, 0, {});
  discovery_a.sender = "NODE_A";
  discovery_a.endpoints = {SEDS_EP_DISCOVERY};
  seds::append_le<uint32_t>(SEDS_EP_TIME_SYNC, discovery_a.payload);
  auto discovery_a_wire = seds::serialize_packet(discovery_a);
  ASSERT_EQ(seds_router_receive_serialized_from_side(source_router.get(), static_cast<uint32_t>(a),
                                                     discovery_a_wire.data(), discovery_a_wire.size()),
            SEDS_OK);

  seds::PacketData discovery_b = seds::make_internal_packet(SEDS_DT_DISCOVERY_ANNOUNCE, 0, {});
  discovery_b.sender = "NODE_B";
  discovery_b.endpoints = {SEDS_EP_DISCOVERY};
  auto discovery_b_wire = seds::serialize_packet(discovery_b);
  ASSERT_EQ(seds_router_receive_serialized_from_side(source_router.get(), static_cast<uint32_t>(b),
                                                     discovery_b_wire.data(), discovery_b_wire.size()),
            SEDS_OK);

  bool did_queue = false;
  clock.now_ms = 100;
  ASSERT_EQ(seds_router_poll_timesync(source_router.get(), &did_queue), SEDS_OK);
  ASSERT_TRUE(did_queue);
  ASSERT_EQ(seds_router_process_tx_queue(source_router.get()), SEDS_OK);
  ASSERT_EQ(side_a.frames.size(), 1u);
  ASSERT_TRUE(side_b.frames.empty());

  Capture req_a{};
  Capture req_b{};
  auto consumer = make_router(Seds_RM_Sink, read_clock, &clock);
  ASSERT_NE(consumer, nullptr);
  const int32_t req_side_a = seds_router_add_side_serialized(consumer.get(), "A", 1, capture_tx, &req_a, false);
  const int32_t req_side_b = seds_router_add_side_serialized(consumer.get(), "B", 1, capture_tx, &req_b, false);
  ASSERT_GE(req_side_a, 0);
  ASSERT_GE(req_side_b, 0);
  ASSERT_EQ(seds_router_configure_timesync(consumer.get(), true, 0u, 50u, 5000u, 100u, 100u), SEDS_OK);
  ASSERT_EQ(seds_router_receive_serialized_from_side(consumer.get(), static_cast<uint32_t>(req_side_a),
                                                     discovery_a_wire.data(), discovery_a_wire.size()),
            SEDS_OK);
  ASSERT_EQ(seds_router_receive_serialized_from_side(consumer.get(), static_cast<uint32_t>(req_side_b),
                                                     discovery_b_wire.data(), discovery_b_wire.size()),
            SEDS_OK);

  seds::PacketData sources_a = seds::make_internal_packet(SEDS_DT_DISCOVERY_TIMESYNC_SOURCES, 0, {});
  sources_a.sender = "NODE_A";
  sources_a.endpoints = {SEDS_EP_DISCOVERY};
  seds::append_le<uint32_t>(1u, sources_a.payload);
  const std::string source_name = "SRC_A";
  seds::append_le<uint32_t>(static_cast<uint32_t>(source_name.size()), sources_a.payload);
  sources_a.payload.insert(sources_a.payload.end(), source_name.begin(), source_name.end());
  auto sources_a_wire = seds::serialize_packet(sources_a);
  ASSERT_EQ(seds_router_receive_serialized_from_side(consumer.get(), static_cast<uint32_t>(req_side_a),
                                                     sources_a_wire.data(), sources_a_wire.size()),
            SEDS_OK);

  seds::PacketData announce_a = seds::make_internal_packet(SEDS_DT_TIME_SYNC_ANNOUNCE, 10, {});
  announce_a.sender = "SRC_A";
  announce_a.endpoints = {SEDS_EP_TIME_SYNC};
  seds::append_le<uint64_t>(1u, announce_a.payload);
  seds::append_le<uint64_t>(1700000000000ull, announce_a.payload);
  auto announce_a_wire = seds::serialize_packet(announce_a);
  ASSERT_EQ(seds_router_receive_serialized_from_side(consumer.get(), static_cast<uint32_t>(req_side_a),
                                                     announce_a_wire.data(), announce_a_wire.size()),
            SEDS_OK);

  did_queue = false;
  clock.now_ms = 200;
  ASSERT_EQ(seds_router_poll_timesync(consumer.get(), &did_queue), SEDS_OK);
  ASSERT_TRUE(did_queue);
  ASSERT_EQ(seds_router_process_tx_queue(consumer.get()), SEDS_OK);
  ASSERT_EQ(req_a.frames.size(), 1u);
  ASSERT_TRUE(req_b.frames.empty());
  auto request = seds::deserialize_packet(req_a.frames.front().data(), req_a.frames.front().size());
  ASSERT_TRUE(request.has_value());
  ASSERT_EQ(request->ty, SEDS_DT_TIME_SYNC_REQUEST);
}

void test_router_remove_side_stops_transmit_and_rejects_removed_ingress() {
  Capture side_a{};
  Capture side_b{};
  const auto router = make_router(Seds_RM_Sink);
  ASSERT_NE(router, nullptr);
  const int32_t a = seds_router_add_side_serialized(router.get(), "A", 1, capture_tx, &side_a, false);
  const int32_t b = seds_router_add_side_serialized(router.get(), "B", 1, capture_tx, &side_b, false);
  ASSERT_GE(a, 0);
  ASSERT_GE(b, 0);

  ASSERT_EQ(seds_router_remove_side(router.get(), a), SEDS_OK);

  const float gps[] = {1.0f, 2.0f, 3.0f};
  const uint32_t endpoints[] = {SEDS_EP_RADIO};
  ASSERT_EQ(seds_router_log_f32(router.get(), SEDS_DT_GPS_DATA, gps, 3), SEDS_OK);
  ASSERT_TRUE(side_a.frames.empty());
  ASSERT_EQ(side_b.frames.size(), 1u);

  const SedsPacketView pkt{
      .ty = SEDS_DT_GPS_DATA,
      .data_size = sizeof(gps),
      .sender = "SRC",
      .sender_len = 3,
      .endpoints = endpoints,
      .num_endpoints = 1,
      .timestamp = 1,
      .payload = reinterpret_cast<const uint8_t *>(gps),
      .payload_len = sizeof(gps),
  };
  ASSERT_EQ(seds_router_receive_from_side(router.get(), static_cast<uint32_t>(a), &pkt), SEDS_INVALID_LINK_ID);
}

void test_router_runtime_routes_support_asymmetric_and_ingress_only_links() {
  Capture side_a{};
  Capture side_b{};
  Capture side_c{};
  const auto router = make_router(Seds_RM_Relay);
  ASSERT_NE(router, nullptr);
  const int32_t a = seds_router_add_side_serialized(router.get(), "A", 1, capture_tx, &side_a, false);
  const int32_t b = seds_router_add_side_serialized(router.get(), "B", 1, capture_tx, &side_b, false);
  const int32_t c = seds_router_add_side_serialized(router.get(), "C", 1, capture_tx, &side_c, false);
  ASSERT_GE(a, 0);
  ASSERT_GE(b, 0);
  ASSERT_GE(c, 0);

  ASSERT_EQ(seds_router_set_route(router.get(), -1, b, false), SEDS_OK);
  ASSERT_EQ(seds_router_set_route(router.get(), -1, c, false), SEDS_OK);
  ASSERT_EQ(seds_router_set_route(router.get(), b, a, false), SEDS_OK);
  ASSERT_EQ(seds_router_set_side_egress_enabled(router.get(), c, false), SEDS_OK);

  const float gps[] = {1.0f, 2.0f, 3.0f};
  const uint32_t endpoints[] = {SEDS_EP_RADIO};
  const SedsPacketView pkt_a{
      .ty = SEDS_DT_GPS_DATA,
      .data_size = sizeof(gps),
      .sender = "SRC",
      .sender_len = 3,
      .endpoints = endpoints,
      .num_endpoints = 1,
      .timestamp = 2,
      .payload = reinterpret_cast<const uint8_t *>(gps),
      .payload_len = sizeof(gps),
  };
  const float gps_b[] = {4.0f, 5.0f, 6.0f};
  const SedsPacketView pkt_b{
      .ty = SEDS_DT_GPS_DATA,
      .data_size = sizeof(gps_b),
      .sender = "SRC",
      .sender_len = 3,
      .endpoints = endpoints,
      .num_endpoints = 1,
      .timestamp = 3,
      .payload = reinterpret_cast<const uint8_t *>(gps_b),
      .payload_len = sizeof(gps_b),
  };
  const float gps_c[] = {7.0f, 8.0f, 9.0f};
  const SedsPacketView pkt_c{
      .ty = SEDS_DT_GPS_DATA,
      .data_size = sizeof(gps_c),
      .sender = "SRC",
      .sender_len = 3,
      .endpoints = endpoints,
      .num_endpoints = 1,
      .timestamp = 4,
      .payload = reinterpret_cast<const uint8_t *>(gps_c),
      .payload_len = sizeof(gps_c),
  };

  ASSERT_EQ(seds_router_log_f32(router.get(), SEDS_DT_GPS_DATA, gps, 3), SEDS_OK);
  ASSERT_EQ(side_a.frames.size(), 1u);
  ASSERT_TRUE(side_b.frames.empty());
  ASSERT_TRUE(side_c.frames.empty());

  side_a.frames.clear();
  side_b.frames.clear();
  side_c.frames.clear();
  ASSERT_EQ(seds_router_receive_from_side(router.get(), static_cast<uint32_t>(a), &pkt_a), SEDS_OK);
  ASSERT_EQ(seds_router_process_all_queues(router.get()), SEDS_OK);
  ASSERT_EQ(side_b.frames.size(), 1u);
  ASSERT_TRUE(side_c.frames.empty());

  side_a.frames.clear();
  side_b.frames.clear();
  side_c.frames.clear();
  ASSERT_EQ(seds_router_receive_from_side(router.get(), static_cast<uint32_t>(b), &pkt_b), SEDS_OK);
  ASSERT_EQ(seds_router_process_all_queues(router.get()), SEDS_OK);
  ASSERT_TRUE(side_a.frames.empty());
  ASSERT_TRUE(side_b.frames.empty());
  ASSERT_TRUE(side_c.frames.empty());

  side_a.frames.clear();
  side_b.frames.clear();
  side_c.frames.clear();
  ASSERT_EQ(seds_router_receive_from_side(router.get(), static_cast<uint32_t>(c), &pkt_c), SEDS_OK);
  ASSERT_EQ(seds_router_process_all_queues(router.get()), SEDS_OK);
  ASSERT_EQ(side_a.frames.size(), 1u);
  ASSERT_EQ(side_b.frames.size(), 1u);
  ASSERT_TRUE(side_c.frames.empty());
}

void test_relay_remove_side_stops_transmit_and_rejects_removed_ingress() {
  Capture side_a{};
  Capture side_b{};
  Capture side_c{};
  const auto relay = make_relay();
  ASSERT_NE(relay, nullptr);
  const int32_t a = seds_relay_add_side_serialized(relay.get(), "A", 1, capture_tx, &side_a, false);
  const int32_t b = seds_relay_add_side_serialized(relay.get(), "B", 1, capture_tx, &side_b, false);
  const int32_t c = seds_relay_add_side_serialized(relay.get(), "C", 1, capture_tx, &side_c, false);
  ASSERT_GE(a, 0);
  ASSERT_GE(b, 0);
  ASSERT_GE(c, 0);

  ASSERT_EQ(seds_relay_remove_side(relay.get(), a), SEDS_OK);

  const float gps[] = {1.0f, 2.0f, 3.0f};
  const uint32_t endpoints[] = {SEDS_EP_RADIO};
  const SedsPacketView pkt{
      .ty = SEDS_DT_GPS_DATA,
      .data_size = sizeof(gps),
      .sender = "SRC",
      .sender_len = 3,
      .endpoints = endpoints,
      .num_endpoints = 1,
      .timestamp = 5,
      .payload = reinterpret_cast<const uint8_t *>(gps),
      .payload_len = sizeof(gps),
  };
  ASSERT_EQ(seds_relay_rx_packet_from_side(relay.get(), static_cast<uint32_t>(b), &pkt), SEDS_OK);
  ASSERT_EQ(seds_relay_process_all_queues(relay.get()), SEDS_OK);
  ASSERT_TRUE(side_a.frames.empty());
  ASSERT_TRUE(side_b.frames.empty());
  ASSERT_EQ(side_c.frames.size(), 1u);
  ASSERT_EQ(seds_relay_rx_packet_from_side(relay.get(), static_cast<uint32_t>(a), &pkt), SEDS_INVALID_LINK_ID);
}

void test_relay_runtime_routes_support_asymmetric_and_ingress_only_links() {
  Capture side_a{};
  Capture side_b{};
  Capture side_c{};
  const auto relay = make_relay();
  ASSERT_NE(relay, nullptr);
  const int32_t a = seds_relay_add_side_serialized(relay.get(), "A", 1, capture_tx, &side_a, false);
  const int32_t b = seds_relay_add_side_serialized(relay.get(), "B", 1, capture_tx, &side_b, false);
  const int32_t c = seds_relay_add_side_serialized(relay.get(), "C", 1, capture_tx, &side_c, false);
  ASSERT_GE(a, 0);
  ASSERT_GE(b, 0);
  ASSERT_GE(c, 0);

  ASSERT_EQ(seds_relay_set_route(relay.get(), b, a, false), SEDS_OK);
  ASSERT_EQ(seds_relay_set_side_egress_enabled(relay.get(), c, false), SEDS_OK);

  const float gps[] = {1.0f, 2.0f, 3.0f};
  const uint32_t endpoints[] = {SEDS_EP_RADIO};
  const SedsPacketView pkt{
      .ty = SEDS_DT_GPS_DATA,
      .data_size = sizeof(gps),
      .sender = "SRC",
      .sender_len = 3,
      .endpoints = endpoints,
      .num_endpoints = 1,
      .timestamp = 1,
      .payload = reinterpret_cast<const uint8_t *>(gps),
      .payload_len = sizeof(gps),
  };

  ASSERT_EQ(seds_relay_rx_packet_from_side(relay.get(), static_cast<uint32_t>(a), &pkt), SEDS_OK);
  ASSERT_EQ(seds_relay_process_all_queues(relay.get()), SEDS_OK);
  ASSERT_EQ(side_b.frames.size(), 1u);
  ASSERT_TRUE(side_c.frames.empty());

  side_a.frames.clear();
  side_b.frames.clear();
  side_c.frames.clear();
  ASSERT_EQ(seds_relay_rx_packet_from_side(relay.get(), static_cast<uint32_t>(b), &pkt), SEDS_OK);
  ASSERT_EQ(seds_relay_process_all_queues(relay.get()), SEDS_OK);
  ASSERT_TRUE(side_a.frames.empty());
  ASSERT_TRUE(side_b.frames.empty());
  ASSERT_TRUE(side_c.frames.empty());

  side_a.frames.clear();
  side_b.frames.clear();
  side_c.frames.clear();
  ASSERT_EQ(seds_relay_rx_packet_from_side(relay.get(), static_cast<uint32_t>(c), &pkt), SEDS_OK);
  ASSERT_EQ(seds_relay_process_all_queues(relay.get()), SEDS_OK);
  ASSERT_EQ(side_a.frames.size(), 1u);
  ASSERT_EQ(side_b.frames.size(), 1u);
  ASSERT_TRUE(side_c.frames.empty());
}

void test_relay_periodic_dispatches_discovery() {
  ManualClock clock{};
  Capture side_a{};
  Capture side_b{};
  SedsRelay *const relay = seds_relay_new(read_clock, &clock);
  ASSERT_NE(relay, nullptr);
  const int32_t a = seds_relay_add_side_serialized(relay, "A", 1, capture_tx, &side_a, false);
  const int32_t b = seds_relay_add_side_serialized(relay, "B", 1, capture_tx, &side_b, false);
  ASSERT_GE(a, 0);
  ASSERT_GE(b, 0);

  seds::PacketData discovery = seds::make_internal_packet(SEDS_DT_DISCOVERY_ANNOUNCE, 0, {});
  discovery.sender = "NODE_A";
  discovery.endpoints = {SEDS_EP_DISCOVERY};
  seds::append_le<uint32_t>(SEDS_EP_RADIO, discovery.payload);
  const auto bytes = seds::serialize_packet(discovery);
  ASSERT_EQ(seds_relay_rx_serialized_from_side(relay, static_cast<uint32_t>(a), bytes.data(), bytes.size()), SEDS_OK);
  ASSERT_EQ(seds_relay_periodic(relay, 0), SEDS_OK);
  side_a.frames.clear();
  side_b.frames.clear();

  ASSERT_EQ(seds_relay_periodic(relay, 0), SEDS_OK);
  ASSERT_FALSE(side_b.frames.empty());
  const auto pkt = seds::deserialize_packet(side_b.frames.front().data(), side_b.frames.front().size());
  ASSERT_TRUE(pkt.has_value());
  ASSERT_EQ(pkt->ty, SEDS_DT_DISCOVERY_ANNOUNCE);

  seds_relay_free(relay);
}

void test_router_c_abi_rejects_reserved_discovery_endpoint_handler() {
  const SedsLocalEndpointDesc desc{
      .endpoint = SEDS_EP_DISCOVERY,
      .packet_handler = count_packet,
      .serialized_handler = nullptr,
      .user = nullptr,
  };
  ASSERT_EQ(seds_router_new(Seds_RM_Sink, nullptr, nullptr, &desc, 1), nullptr);
}

void test_router_c_abi_rejects_reserved_timesync_endpoint_handler() {
  const SedsLocalEndpointDesc desc{
      .endpoint = SEDS_EP_TIME_SYNC,
      .packet_handler = count_packet,
      .serialized_handler = nullptr,
      .user = nullptr,
  };
  ASSERT_EQ(seds_router_new(Seds_RM_Sink, nullptr, nullptr, &desc, 1), nullptr);
}

void test_router_typed_routes_still_respect_base_route_disables() {
  Capture side_a{};
  Capture side_b{};
  const auto router = make_router(Seds_RM_Relay);
  ASSERT_NE(router, nullptr);
  const int32_t a = seds_router_add_side_serialized(router.get(), "A", 1, capture_tx, &side_a, false);
  const int32_t b = seds_router_add_side_serialized(router.get(), "B", 1, capture_tx, &side_b, false);
  ASSERT_GE(a, 0);
  ASSERT_GE(b, 0);

  ASSERT_EQ(seds_router_set_typed_route(router.get(), -1, SEDS_DT_GPS_DATA, b, true), SEDS_OK);
  ASSERT_EQ(seds_router_set_route(router.get(), -1, b, false), SEDS_OK);

  const float gps[] = {1.0f, 2.0f, 3.0f};
  ASSERT_EQ(seds_router_log_f32(router.get(), SEDS_DT_GPS_DATA, gps, 3), SEDS_OK);
  ASSERT_TRUE(side_a.frames.empty());
  ASSERT_TRUE(side_b.frames.empty());
}

void test_router_weighted_route_mode_splits_discovered_paths_by_weight() {
  Capture side_a{};
  Capture side_b{};
  const auto router = make_router(Seds_RM_Sink);
  ASSERT_NE(router, nullptr);
  const int32_t a = seds_router_add_side_serialized(router.get(), "A", 1, capture_tx, &side_a, false);
  const int32_t b = seds_router_add_side_serialized(router.get(), "B", 1, capture_tx, &side_b, false);
  ASSERT_GE(a, 0);
  ASSERT_GE(b, 0);

  seds::PacketData discovery = seds::make_internal_packet(SEDS_DT_DISCOVERY_ANNOUNCE, 0, {});
  discovery.endpoints = {SEDS_EP_DISCOVERY};
  discovery.sender = "REMOTE_A";
  seds::append_le<uint32_t>(SEDS_EP_RADIO, discovery.payload);
  auto bytes = seds::serialize_packet(discovery);
  ASSERT_EQ(
      seds_router_receive_serialized_from_side(router.get(), static_cast<uint32_t>(a), bytes.data(), bytes.size()),
      SEDS_OK);
  discovery.sender = "REMOTE_B";
  discovery.timestamp = 1;
  bytes = seds::serialize_packet(discovery);
  ASSERT_EQ(
      seds_router_receive_serialized_from_side(router.get(), static_cast<uint32_t>(b), bytes.data(), bytes.size()),
      SEDS_OK);
  side_a.frames.clear();
  side_b.frames.clear();

  ASSERT_EQ(seds_router_set_source_route_mode(router.get(), -1, Seds_RSM_Weighted), SEDS_OK);
  ASSERT_EQ(seds_router_set_route_weight(router.get(), -1, a, 2), SEDS_OK);
  ASSERT_EQ(seds_router_set_route_weight(router.get(), -1, b, 1), SEDS_OK);

  for (uint64_t seq = 0; seq < 6; ++seq) {
    const float gps[] = {static_cast<float>(seq), static_cast<float>(seq + 1), static_cast<float>(seq + 2)};
    ASSERT_EQ(seds_router_log_f32(router.get(), SEDS_DT_GPS_DATA, gps, 3), SEDS_OK);
  }
  ASSERT_EQ(side_a.frames.size(), 4u);
  ASSERT_EQ(side_b.frames.size(), 2u);
}

void test_router_weighted_route_mode_falls_back_to_remaining_path_when_other_path_expires() {
  ManualClock clock{};
  Capture side_a{};
  Capture side_b{};
  const auto router = make_router(Seds_RM_Sink, read_clock, &clock);
  ASSERT_NE(router, nullptr);
  const int32_t a = seds_router_add_side_serialized(router.get(), "A", 1, capture_tx, &side_a, false);
  const int32_t b = seds_router_add_side_serialized(router.get(), "B", 1, capture_tx, &side_b, false);
  ASSERT_GE(a, 0);
  ASSERT_GE(b, 0);

  seds::PacketData discovery = seds::make_internal_packet(SEDS_DT_DISCOVERY_ANNOUNCE, 0, {});
  discovery.endpoints = {SEDS_EP_DISCOVERY};
  discovery.sender = "REMOTE_A";
  seds::append_le<uint32_t>(SEDS_EP_RADIO, discovery.payload);
  auto bytes = seds::serialize_packet(discovery);
  ASSERT_EQ(
      seds_router_receive_serialized_from_side(router.get(), static_cast<uint32_t>(a), bytes.data(), bytes.size()),
      SEDS_OK);
  clock.now_ms = seds::kDiscoveryTtlMs / 2;
  discovery.sender = "REMOTE_B";
  discovery.timestamp = clock.now_ms;
  bytes = seds::serialize_packet(discovery);
  ASSERT_EQ(
      seds_router_receive_serialized_from_side(router.get(), static_cast<uint32_t>(b), bytes.data(), bytes.size()),
      SEDS_OK);
  side_a.frames.clear();
  side_b.frames.clear();

  ASSERT_EQ(seds_router_set_source_route_mode(router.get(), -1, Seds_RSM_Weighted), SEDS_OK);
  ASSERT_EQ(seds_router_set_route_weight(router.get(), -1, a, 1), SEDS_OK);
  ASSERT_EQ(seds_router_set_route_weight(router.get(), -1, b, 1), SEDS_OK);

  for (uint64_t seq = 0; seq < 2; ++seq) {
    const float gps[] = {static_cast<float>(seq), static_cast<float>(seq + 1), static_cast<float>(seq + 2)};
    ASSERT_EQ(seds_router_log_f32(router.get(), SEDS_DT_GPS_DATA, gps, 3), SEDS_OK);
  }
  const auto before_a = side_a.frames.size();
  const auto before_b = side_b.frames.size();
  ASSERT_EQ(before_a + before_b, 2u);

  clock.now_ms = seds::kDiscoveryTtlMs + 1;
  const float gps3[] = {9.0f, 10.0f, 11.0f};
  ASSERT_EQ(seds_router_log_f32(router.get(), SEDS_DT_GPS_DATA, gps3, 3), SEDS_OK);
  ASSERT_EQ(side_a.frames.size(), before_a);
  ASSERT_EQ(side_b.frames.size(), before_b + 1u);
}

void test_router_failover_route_mode_switches_when_preferred_side_is_removed() {
  Capture side_a{};
  Capture side_b{};
  const auto router = make_router(Seds_RM_Sink);
  ASSERT_NE(router, nullptr);
  const int32_t a = seds_router_add_side_serialized(router.get(), "A", 1, capture_tx, &side_a, false);
  const int32_t b = seds_router_add_side_serialized(router.get(), "B", 1, capture_tx, &side_b, false);
  ASSERT_GE(a, 0);
  ASSERT_GE(b, 0);

  seds::PacketData discovery = seds::make_internal_packet(SEDS_DT_DISCOVERY_ANNOUNCE, 0, {});
  discovery.endpoints = {SEDS_EP_DISCOVERY};
  discovery.sender = "REMOTE_A";
  seds::append_le<uint32_t>(SEDS_EP_RADIO, discovery.payload);
  auto bytes = seds::serialize_packet(discovery);
  ASSERT_EQ(
      seds_router_receive_serialized_from_side(router.get(), static_cast<uint32_t>(a), bytes.data(), bytes.size()),
      SEDS_OK);
  discovery.sender = "REMOTE_B";
  discovery.timestamp = 1;
  bytes = seds::serialize_packet(discovery);
  ASSERT_EQ(
      seds_router_receive_serialized_from_side(router.get(), static_cast<uint32_t>(b), bytes.data(), bytes.size()),
      SEDS_OK);
  side_a.frames.clear();
  side_b.frames.clear();

  ASSERT_EQ(seds_router_set_source_route_mode(router.get(), -1, Seds_RSM_Failover), SEDS_OK);
  ASSERT_EQ(seds_router_set_route_priority(router.get(), -1, a, 0), SEDS_OK);
  ASSERT_EQ(seds_router_set_route_priority(router.get(), -1, b, 1), SEDS_OK);

  const float gps1[] = {1.0f, 2.0f, 3.0f};
  ASSERT_EQ(seds_router_log_f32(router.get(), SEDS_DT_GPS_DATA, gps1, 3), SEDS_OK);
  ASSERT_EQ(side_a.frames.size(), 1u);
  ASSERT_TRUE(side_b.frames.empty());

  ASSERT_EQ(seds_router_remove_side(router.get(), a), SEDS_OK);
  const float gps2[] = {4.0f, 5.0f, 6.0f};
  ASSERT_EQ(seds_router_log_f32(router.get(), SEDS_DT_GPS_DATA, gps2, 3), SEDS_OK);
  ASSERT_EQ(side_a.frames.size(), 1u);
  ASSERT_EQ(side_b.frames.size(), 1u);
}

void test_relay_weighted_route_mode_splits_discovered_paths_by_weight() {
  Capture side_a{};
  Capture side_b{};
  Capture side_c{};
  const auto relay = make_relay();
  ASSERT_NE(relay, nullptr);
  const int32_t a = seds_relay_add_side_serialized(relay.get(), "A", 1, capture_tx, &side_a, false);
  const int32_t b = seds_relay_add_side_serialized(relay.get(), "B", 1, capture_tx, &side_b, false);
  const int32_t c = seds_relay_add_side_serialized(relay.get(), "C", 1, capture_tx, &side_c, false);
  ASSERT_GE(a, 0);
  ASSERT_GE(b, 0);
  ASSERT_GE(c, 0);

  seds::PacketData discovery = seds::make_internal_packet(SEDS_DT_DISCOVERY_ANNOUNCE, 0, {});
  discovery.endpoints = {SEDS_EP_DISCOVERY};
  discovery.sender = "REMOTE_A";
  seds::append_le<uint32_t>(SEDS_EP_RADIO, discovery.payload);
  auto bytes = seds::serialize_packet(discovery);
  ASSERT_EQ(seds_relay_rx_serialized_from_side(relay.get(), static_cast<uint32_t>(a), bytes.data(), bytes.size()),
            SEDS_OK);
  discovery.sender = "REMOTE_B";
  discovery.timestamp = 1;
  bytes = seds::serialize_packet(discovery);
  ASSERT_EQ(seds_relay_rx_serialized_from_side(relay.get(), static_cast<uint32_t>(b), bytes.data(), bytes.size()),
            SEDS_OK);
  ASSERT_EQ(seds_relay_process_all_queues(relay.get()), SEDS_OK);
  side_a.frames.clear();
  side_b.frames.clear();

  ASSERT_EQ(seds_relay_set_source_route_mode(relay.get(), c, Seds_RSM_Weighted), SEDS_OK);
  ASSERT_EQ(seds_relay_set_route_weight(relay.get(), c, a, 2), SEDS_OK);
  ASSERT_EQ(seds_relay_set_route_weight(relay.get(), c, b, 1), SEDS_OK);

  const uint32_t endpoints[] = {SEDS_EP_RADIO};
  for (uint64_t seq = 0; seq < 6; ++seq) {
    const float gps[] = {static_cast<float>(seq), static_cast<float>(seq + 1), static_cast<float>(seq + 2)};
    const SedsPacketView pkt{SEDS_DT_GPS_DATA, sizeof(gps), "SRC", 3,
                             endpoints,        1,           seq,   reinterpret_cast<const uint8_t *>(gps),
                             sizeof(gps)};
    ASSERT_EQ(seds_relay_rx_packet_from_side(relay.get(), static_cast<uint32_t>(c), &pkt), SEDS_OK);
  }
  ASSERT_EQ(seds_relay_process_all_queues(relay.get()), SEDS_OK);
  ASSERT_EQ(side_a.frames.size(), 4u);
  ASSERT_EQ(side_b.frames.size(), 2u);
}

void test_relay_weighted_route_mode_falls_back_to_remaining_path_when_other_path_expires() {
  ManualClock clock{};
  Capture side_a{};
  Capture side_b{};
  Capture side_c{};
  const auto relay = make_relay(read_clock, &clock);
  ASSERT_NE(relay, nullptr);
  const int32_t a = seds_relay_add_side_serialized(relay.get(), "A", 1, capture_tx, &side_a, false);
  const int32_t b = seds_relay_add_side_serialized(relay.get(), "B", 1, capture_tx, &side_b, false);
  const int32_t c = seds_relay_add_side_serialized(relay.get(), "C", 1, capture_tx, &side_c, false);
  ASSERT_GE(a, 0);
  ASSERT_GE(b, 0);
  ASSERT_GE(c, 0);

  seds::PacketData discovery = seds::make_internal_packet(SEDS_DT_DISCOVERY_ANNOUNCE, 0, {});
  discovery.endpoints = {SEDS_EP_DISCOVERY};
  discovery.sender = "REMOTE_A";
  seds::append_le<uint32_t>(SEDS_EP_RADIO, discovery.payload);
  auto bytes = seds::serialize_packet(discovery);
  ASSERT_EQ(seds_relay_rx_serialized_from_side(relay.get(), static_cast<uint32_t>(a), bytes.data(), bytes.size()),
            SEDS_OK);
  ASSERT_EQ(seds_relay_process_all_queues(relay.get()), SEDS_OK);
  clock.now_ms = seds::kDiscoveryTtlMs / 2;
  discovery.sender = "REMOTE_B";
  discovery.timestamp = clock.now_ms;
  bytes = seds::serialize_packet(discovery);
  ASSERT_EQ(seds_relay_rx_serialized_from_side(relay.get(), static_cast<uint32_t>(b), bytes.data(), bytes.size()),
            SEDS_OK);
  ASSERT_EQ(seds_relay_process_all_queues(relay.get()), SEDS_OK);
  side_a.frames.clear();
  side_b.frames.clear();

  ASSERT_EQ(seds_relay_set_source_route_mode(relay.get(), c, Seds_RSM_Weighted), SEDS_OK);
  ASSERT_EQ(seds_relay_set_route_weight(relay.get(), c, a, 1), SEDS_OK);
  ASSERT_EQ(seds_relay_set_route_weight(relay.get(), c, b, 1), SEDS_OK);

  const uint32_t endpoints[] = {SEDS_EP_RADIO};
  for (uint64_t seq = 0; seq < 2; ++seq) {
    const float gps[] = {static_cast<float>(seq), static_cast<float>(seq + 1), static_cast<float>(seq + 2)};
    const SedsPacketView pkt{SEDS_DT_GPS_DATA, sizeof(gps), "SRC", 3,
                             endpoints,        1,           seq,   reinterpret_cast<const uint8_t *>(gps),
                             sizeof(gps)};
    ASSERT_EQ(seds_relay_rx_packet_from_side(relay.get(), static_cast<uint32_t>(c), &pkt), SEDS_OK);
  }
  ASSERT_EQ(seds_relay_process_all_queues(relay.get()), SEDS_OK);
  const auto before_a = side_a.frames.size();
  const auto before_b = side_b.frames.size();
  ASSERT_EQ(before_a + before_b, 2u);

  clock.now_ms = seds::kDiscoveryTtlMs + 1;
  const float gps3[] = {9.0f, 10.0f, 11.0f};
  const SedsPacketView pkt3{
      SEDS_DT_GPS_DATA, sizeof(gps3), "SRC", 3, endpoints, 1, 3, reinterpret_cast<const uint8_t *>(gps3), sizeof(gps3)};
  ASSERT_EQ(seds_relay_rx_packet_from_side(relay.get(), static_cast<uint32_t>(c), &pkt3), SEDS_OK);
  ASSERT_EQ(seds_relay_process_all_queues(relay.get()), SEDS_OK);
  ASSERT_EQ(side_a.frames.size(), before_a);
  ASSERT_EQ(side_b.frames.size(), before_b + 1u);
}

void test_relay_failover_route_mode_switches_when_preferred_side_is_removed() {
  Capture side_a{};
  Capture side_b{};
  Capture side_c{};
  const auto relay = make_relay();
  ASSERT_NE(relay, nullptr);
  const int32_t a = seds_relay_add_side_serialized(relay.get(), "A", 1, capture_tx, &side_a, false);
  const int32_t b = seds_relay_add_side_serialized(relay.get(), "B", 1, capture_tx, &side_b, false);
  const int32_t c = seds_relay_add_side_serialized(relay.get(), "C", 1, capture_tx, &side_c, false);
  ASSERT_GE(a, 0);
  ASSERT_GE(b, 0);
  ASSERT_GE(c, 0);

  seds::PacketData discovery = seds::make_internal_packet(SEDS_DT_DISCOVERY_ANNOUNCE, 0, {});
  discovery.endpoints = {SEDS_EP_DISCOVERY};
  discovery.sender = "REMOTE_A";
  seds::append_le<uint32_t>(SEDS_EP_RADIO, discovery.payload);
  auto bytes = seds::serialize_packet(discovery);
  ASSERT_EQ(seds_relay_rx_serialized_from_side(relay.get(), static_cast<uint32_t>(a), bytes.data(), bytes.size()),
            SEDS_OK);
  discovery.sender = "REMOTE_B";
  discovery.timestamp = 1;
  bytes = seds::serialize_packet(discovery);
  ASSERT_EQ(seds_relay_rx_serialized_from_side(relay.get(), static_cast<uint32_t>(b), bytes.data(), bytes.size()),
            SEDS_OK);
  ASSERT_EQ(seds_relay_process_all_queues(relay.get()), SEDS_OK);
  side_a.frames.clear();
  side_b.frames.clear();

  ASSERT_EQ(seds_relay_set_source_route_mode(relay.get(), c, Seds_RSM_Failover), SEDS_OK);
  ASSERT_EQ(seds_relay_set_route_priority(relay.get(), c, a, 0), SEDS_OK);
  ASSERT_EQ(seds_relay_set_route_priority(relay.get(), c, b, 1), SEDS_OK);

  const uint32_t endpoints[] = {SEDS_EP_RADIO};
  const float gps1[] = {1.0f, 2.0f, 3.0f};
  const SedsPacketView pkt1{
      SEDS_DT_GPS_DATA, sizeof(gps1), "SRC", 3, endpoints, 1, 1, reinterpret_cast<const uint8_t *>(gps1), sizeof(gps1)};
  ASSERT_EQ(seds_relay_rx_packet_from_side(relay.get(), static_cast<uint32_t>(c), &pkt1), SEDS_OK);
  ASSERT_EQ(seds_relay_process_all_queues(relay.get()), SEDS_OK);
  ASSERT_EQ(side_a.frames.size(), 1u);
  ASSERT_TRUE(side_b.frames.empty());

  ASSERT_EQ(seds_relay_remove_side(relay.get(), a), SEDS_OK);
  const float gps2[] = {4.0f, 5.0f, 6.0f};
  const SedsPacketView pkt2{
      SEDS_DT_GPS_DATA, sizeof(gps2), "SRC", 3, endpoints, 1, 2, reinterpret_cast<const uint8_t *>(gps2), sizeof(gps2)};
  ASSERT_EQ(seds_relay_rx_packet_from_side(relay.get(), static_cast<uint32_t>(c), &pkt2), SEDS_OK);
  ASSERT_EQ(seds_relay_process_all_queues(relay.get()), SEDS_OK);
  ASSERT_EQ(side_a.frames.size(), 1u);
  ASSERT_EQ(side_b.frames.size(), 1u);
}

void test_multibus_system_flow() {
  SimBus bus1{};
  SimBus bus2{};
  SedsRelay *relay = seds_relay_new(nullptr, nullptr);
  assert(relay);
  bus1.relay = relay;
  bus2.relay = relay;
  bus1.relay_side = static_cast<uint32_t>(seds_relay_add_side_serialized(relay, "bus1", 4, sim_relay_tx, &bus1, false));
  bus2.relay_side = static_cast<uint32_t>(seds_relay_add_side_serialized(relay, "bus2", 4, sim_relay_tx, &bus2, false));

  SimNode a{}, b{}, c{}, d{};
  a.bus = &bus1;
  b.bus = &bus1;
  c.bus = &bus1;
  d.bus = &bus2;
  const SedsLocalEndpointDesc a_handlers[] = {{SEDS_EP_RADIO, sim_radio_handler, nullptr, &a}};
  const SedsLocalEndpointDesc b_handlers[] = {{SEDS_EP_SD_CARD, sim_sd_handler, nullptr, &b}};
  const SedsLocalEndpointDesc d_handlers[] = {{SEDS_EP_RADIO, sim_radio_handler, nullptr, &d}};
  a.router = seds_router_new(Seds_RM_Sink, nullptr, nullptr, a_handlers, 1);
  b.router = seds_router_new(Seds_RM_Sink, nullptr, nullptr, b_handlers, 1);
  c.router = seds_router_new(Seds_RM_Sink, nullptr, nullptr, nullptr, 0);
  d.router = seds_router_new(Seds_RM_Sink, nullptr, nullptr, d_handlers, 1);
  assert(a.router && b.router && c.router && d.router);
  a.side_id = static_cast<uint32_t>(seds_router_add_side_serialized(a.router, "BUS", 3, sim_node_tx, &a, false));
  b.side_id = static_cast<uint32_t>(seds_router_add_side_serialized(b.router, "BUS", 3, sim_node_tx, &b, false));
  c.side_id = static_cast<uint32_t>(seds_router_add_side_serialized(c.router, "BUS", 3, sim_node_tx, &c, false));
  d.side_id = static_cast<uint32_t>(seds_router_add_side_serialized(d.router, "BUS", 3, sim_node_tx, &d, false));
  bus1.nodes = {&a, &b, &c};
  bus2.nodes = {&d};

  const float gps[] = {1.0f, 2.0f, 3.0f};
  const float imu[] = {1, 2, 3, 4, 5, 6};
  const float batt[] = {7.0f, 8.0f};
  assert(seds_router_log_f32(a.router, SEDS_DT_GPS_DATA, gps, 3) == SEDS_OK);
  assert(seds_router_log_f32(b.router, SEDS_DT_IMU_DATA, imu, 6) == SEDS_OK);
  assert(seds_router_log_f32(c.router, SEDS_DT_BATTERY_STATUS, batt, 2) == SEDS_OK);

  for (int i = 0; i < 6; ++i) {
    pump_bus(bus1);
    pump_bus(bus2);
    seds_router_process_all_queues(a.router);
    seds_router_process_all_queues(b.router);
    seds_router_process_all_queues(c.router);
    seds_router_process_all_queues(d.router);
    seds_relay_process_all_queues(relay);
  }

  assert(a.radio_hits >= 2);
  assert(b.sd_hits >= 2);
  assert(d.radio_hits >= 1);

  seds_router_free(a.router);
  seds_router_free(b.router);
  seds_router_free(c.router);
  seds_router_free(d.router);
  seds_relay_free(relay);
}

} // namespace

TEST(PacketTest, RoundtripAndCompression) { test_packet_roundtrip_and_compression(); }
TEST(PacketTest, SerializeRoundtripGps) { test_packet_roundtrip_gps_exact(); }
TEST(PacketTest, StringFormatting) { test_packet_string_formatting(); }
TEST(PacketTest, HeaderStringMatchesExpectation) { test_header_string_matches_expectation(); }
TEST(PacketTest, DataAsF32RoundtripsGps) { test_data_as_f32_roundtrips_gps(); }
TEST(PacketTest, MismatchedTypedAccessorReturnsTypeMismatch) { test_mismatched_typed_accessor_returns_type_mismatch(); }
TEST(PacketTest, DataAsBoolDecodesNonzero) { test_data_as_bool_decodes_nonzero(); }
TEST(PacketTest, StringGetterTrimsTrailingNuls) { test_string_getter_trims_trailing_nuls(); }
TEST(PacketTest, HeaderSizeIsPrefixOfWireImage) { test_header_size_is_prefix_of_wire_image(); }
TEST(PacketTest, HexStringMatchesExpectation) { test_packet_hex_string_matches_expectation(); }
TEST(PacketTest, ErrorEnumCodeRoundtripAndStrings) { test_error_enum_code_roundtrip_and_strings(); }
TEST(PacketTest, CAbiTopologyExportMatchesTopologySnapshotShape) {
  test_c_abi_topology_export_matches_topology_snapshot_shape();
}
TEST(PacketTest, DeserializeHeaderOnlyShortBufferFails) { test_deserialize_header_only_short_buffer_fails(); }
TEST(PacketTest, DeserializeHeaderOnlyThenFullParseMatches) { test_deserialize_header_only_then_full_parse_matches(); }
TEST(PacketTest, ValidateRejectsEmptyEndpointsAndSizeMismatch) {
  test_packet_validate_rejects_empty_endpoints_and_size_mismatch();
}
TEST(PacketTest, WireSizeMatchesSerializedLen) { test_packet_wire_size_matches_serialized_len(); }
TEST(PacketTest, ErrorPayloadIsTruncatedToMetaSize) { test_error_payload_is_truncated_to_meta_size(); }
TEST(PacketTest, DeserializePacketRejectsOverflowedVarint) { test_deserialize_packet_rejects_overflowed_varint(); }
TEST(PacketTest, SerializerIsCanonicalRoundtrip) { test_serializer_is_canonical_roundtrip(); }
TEST(PacketTest, SerializerVarintScalarsGrowAsExpected) { test_serializer_varint_scalars_grow_as_expected(); }
TEST(PacketTest, EndpointsBitpackRoundtripManyAndExtremes) { test_endpoints_bitpack_roundtrip_many_and_extremes(); }
TEST(PacketTest, PeekEnvelopeMatchesFullParseOnLargeValues) { test_peek_envelope_matches_full_parse_on_large_values(); }
TEST(PacketTest, SerializePacketIsOrderInvariantForEndpoints) {
  test_serialize_packet_is_order_invariant_for_endpoints();
}
TEST(PacketTest, FromF32SliceBuildsValidPacket) { test_from_f32_slice_builds_valid_packet(); }
TEST(PacketTest, FromNoDataBuildsValidPacket) { test_from_no_data_builds_valid_packet(); }
TEST(PacketTest, SerializeValidationEdges) { test_serialize_validation_edges(); }
TEST(PacketTest, ReliableWireHelpers) { test_reliable_wire_helpers(); }

TEST(RouterTest, CAbiDelivery) { test_router_c_abi_delivery(); }
TEST(RouterTest, SendsAndReceives) { test_router_sends_and_receives(); }
TEST(RouterTest, CAbiRejectsReservedDiscoveryEndpointHandler) {
  test_router_c_abi_rejects_reserved_discovery_endpoint_handler();
}
TEST(RouterTest, CAbiRejectsReservedTimeSyncEndpointHandler) {
  test_router_c_abi_rejects_reserved_timesync_endpoint_handler();
}
TEST(RouterTest, ReceiveSerializedPreservesOriginalWireForLocalSerializedHandlers) {
  test_receive_serialized_preserves_original_wire_for_local_serialized_handlers();
}
TEST(RouterTest, SerializedOnlyHandlersDoNotDeserialize) { test_serialized_only_handlers_do_not_deserialize(); }
TEST(RouterTest, PacketAndSerializedHandlersFanOutOnce) { test_packet_and_serialized_handlers_fan_out_once(); }
TEST(RouterTest, SendAvoidsTxWhenOnlyLocalPacketHandlersExist) {
  test_send_avoids_tx_when_only_local_packet_handlers_exist();
}
TEST(RouterTest, ReceiveDirectPacketInvokesHandlers) { test_receive_direct_packet_invokes_handlers(); }
TEST(RouterTest, QueuedRoundtripBetweenTwoRouters) { test_queued_roundtrip_between_two_routers(); }
TEST(RouterTest, QueuedSelfDeliveryViaReceiveQueue) { test_queued_self_delivery_via_receive_queue(); }
TEST(RouterTest, ReceiveSerializedQueueDeliversToSerializedHandlers) {
  test_receive_serialized_queue_delivers_to_serialized_handlers();
}
TEST(RouterTest, TxFailureEmitsLocalErrorPacket) { test_tx_failure_emits_local_error_packet(); }
TEST(RouterTest, TxFailureSendsErrorPacketToAllLocalEndpoints) {
  test_tx_failure_sends_error_packet_to_all_local_endpoints();
}
TEST(RouterTest, LocalHandlerFailureSendsErrorPacketToOtherLocals) {
  test_local_handler_failure_sends_error_packet_to_other_locals();
}
TEST(RouterTest, LocalHandlerRetryAttemptsAreThree) { test_local_handler_retry_attempts_are_three(); }
TEST(RouterTest, ProcessAllQueuesHandlesU64Wraparound) { test_process_all_queues_handles_u64_wraparound(); }
TEST(RouterTest, ProcessAllQueuesTimeoutZeroDrainsFully) { test_process_all_queues_timeout_zero_drains_fully(); }
TEST(RouterTest, ProcessAllQueuesRespectsNonzeroTimeoutBudgetOneReceiveOneSend) {
  test_process_all_queues_respects_nonzero_timeout_budget_one_receive_one_send();
}
TEST(RouterTest, ProcessAllQueuesRespectsNonzeroTimeoutBudgetTwoReceiveOneSend) {
  test_process_all_queues_respects_nonzero_timeout_budget_two_receive_one_send();
}
TEST(RouterTest, ProcessAllQueuesTimeoutDoesNotStarveRxAfterSlowTx) {
  test_process_all_queues_timeout_does_not_starve_rx_after_slow_tx();
}
TEST(RouterTest, ProcessAllQueuesTimeoutZeroHandlesLargeQueues) {
  test_process_all_queues_timeout_zero_handles_large_queues();
}
TEST(RouterTest, HandlerCanReenterRouterWithoutDeadlock) { test_handler_can_reenter_router_without_deadlock(); }
TEST(RouterTest, ConcurrentReceiveSerializedIsThreadSafe) { test_concurrent_receive_serialized_is_thread_safe(); }
TEST(RouterTest, ConcurrentLoggingAndProcessingIsThreadSafe) {
  test_concurrent_logging_and_processing_is_thread_safe();
}
TEST(RouterTest, ConcurrentLogReceiveAndProcessMixIsThreadSafe) {
  test_concurrent_log_receive_and_process_mix_is_thread_safe();
}
TEST(RouterTest, DiscoveryAndTimeSync) { test_discovery_and_timesync(); }
TEST(RouterTest, PeriodicAndDiscoveryPoll) { test_periodic_and_discovery_poll(); }
TEST(RouterTest, PeriodicNoTimeSyncSkipsTimeSyncButKeepsDiscovery) {
  test_periodic_no_timesync_skips_timesync_but_keeps_discovery();
}
TEST(RouterTest, QueuedTimeSyncPrecedesNormalTelemetry) { test_queued_timesync_precedes_normal_telemetry(); }
TEST(RouterTest, DiscoveryAdvertisesTimeSyncEndpointAndSources) {
  test_discovery_advertises_timesync_endpoint_and_sources();
}
TEST(RouterTest, TimeSyncUsesDiscoveryCandidatesAndExactSourceRoute) {
  test_timesync_packets_use_discovery_candidates_and_exact_source_route();
}
TEST(RouterTest, TimeSyncFailoverMonotonic) { test_timesync_failover_monotonic(); }
TEST(RouterTest, LocalTimeSyncSetters) { test_local_timesync_setters(); }
TEST(RouterTest, ReliableFlow) { test_reliable_router_flow(); }
TEST(RouterTest, ReliableRetransmitTimeout) { test_reliable_retransmit_timeout(); }
TEST(RouterTest, ReliableOrderedDeliveryRequiresRetransmit) { test_reliable_ordered_delivery_requires_retransmit(); }
TEST(RouterTest, ReliableLinkRecoversFromDroppedFrames) { test_reliable_link_recovers_from_dropped_frames(); }
TEST(RouterTest, EndToEndReliableAckRoutesBackWithoutFlooding) {
  test_end_to_end_reliable_ack_routes_back_without_flooding();
}
TEST(RouterTest, EndToEndReliableWaitsForAllDiscoveredHolders) {
  test_end_to_end_reliable_waits_for_all_discovered_holders();
}
TEST(RouterTest, ClearQueuesPreventsFurtherProcessing) { test_router_clear_queues_prevents_further_processing(); }
TEST(RouterTest, CanDisableIngressForSide) { test_router_can_disable_ingress_for_side(); }
TEST(RouterTest, ReceiveSerializedDeduplicatesIdenticalFrames) {
  test_router_rx_serialized_deduplicates_identical_frames();
}
TEST(RouterTest, ReceiveSerializedDedupPersistsAcrossTimeAdvance) {
  test_router_rx_serialized_dedup_persists_across_time_advance();
}
TEST(RouterTest, ReceiveSerializedDoesNotDedupeDifferentFrames) {
  test_router_rx_serialized_does_not_dedupe_different_frames();
}
TEST(RouterTest, ReliableDisabledSkipsAck) { test_reliable_disabled_skips_ack(); }
TEST(RouterTest, ReliableSenderDoesNotBlockWhileWaitingForAck) {
  test_reliable_sender_does_not_block_while_waiting_for_ack();
}
TEST(RouterTest, SideEnableDisableAndRemove) { test_side_enable_disable_and_remove(); }
TEST(RouterTest, RouteControls) { test_router_route_controls(); }
TEST(RouterTest, TypedRoutesCanTargetOneOrManySides) { test_router_typed_routes_can_target_one_or_many_sides(); }
TEST(RouterTest, BoundedQueueBehavior) { test_bounded_queue_behavior(); }
TEST(RouterTest, DiscoverySelfIgnoreAndPerSideAdvertise) { test_discovery_self_ignore_and_per_side_advertise(); }
TEST(RouterTest, SenderIdentityMatchesOutboundProtocol) { test_router_sender_identity_matches_outbound_protocol(); }
TEST(RouterTest, DiscoveryRoutesForOutboundPackets) { test_discovery_routes_for_outbound_packets(); }
TEST(RouterTest, RemoveSideStopsTransmitAndRejectsRemovedIngress) {
  test_router_remove_side_stops_transmit_and_rejects_removed_ingress();
}
TEST(RouterTest, RemoveSideUpdatesDiscoveryRoutesAndAnnouncesRemainingTopology) {
  test_router_remove_side_updates_discovery_routes_and_announces_remaining_topology();
}
TEST(RouterTest, ExportsBoardGraphAndTracksTransitiveEndpointHolders) {
  test_router_exports_board_graph_and_tracks_transitive_endpoint_holders();
}
TEST(RouterTest, RuntimeRoutesSupportAsymmetricAndIngressOnlyLinks) {
  test_router_runtime_routes_support_asymmetric_and_ingress_only_links();
}
TEST(RouterTest, TypedRoutesStillRespectBaseRouteDisables) {
  test_router_typed_routes_still_respect_base_route_disables();
}
TEST(RouterTest, WeightedRouteModeSplitsDiscoveredPathsByWeight) {
  test_router_weighted_route_mode_splits_discovered_paths_by_weight();
}
TEST(RouterTest, FailoverRouteModeSwitchesWhenPreferredPathExpires) {
  test_router_failover_route_mode_switches_when_preferred_path_expires();
}
TEST(RouterTest, WeightedRouteModeFallsBackToRemainingPathWhenOtherPathExpires) {
  test_router_weighted_route_mode_falls_back_to_remaining_path_when_other_path_expires();
}
TEST(RouterTest, FailoverRouteModeSwitchesWhenPreferredSideIsRemoved) {
  test_router_failover_route_mode_switches_when_preferred_side_is_removed();
}
TEST(RouterTest, QueuedDiscoveryPrecedesNormalTelemetry) { test_queued_discovery_precedes_normal_telemetry(); }

TEST(RelayTest, ClearQueuesDropsPendingWork) { test_relay_clear_queues_drops_pending_work(); }
TEST(RelayTest, BasicFanOut) { test_relay_basic_fan_out(); }
TEST(RelayTest, InvalidSideIdReturnsError) { test_relay_invalid_side_id_returns_error(); }
TEST(RelayTest, CanDisableIngressForSide) { test_relay_can_disable_ingress_for_side(); }
TEST(RelayTest, ReliableSeqAdvancesWithAck) { test_relay_reliable_seq_advances_with_ack(); }
TEST(RelayTest, ReliableReordersOutOfOrderFrames) { test_relay_reliable_reorders_out_of_order_frames(); }
TEST(RelayTest, DeduplicatesIdenticalFramesPerSide) { test_relay_deduplicates_identical_frames_per_side(); }
TEST(RelayTest, DedupPersistsAcrossTimeAdvance) { test_relay_dedup_persists_across_time_advance(); }
TEST(RelayTest, DoesNotDedupeDifferentFramesFromSameSide) {
  test_relay_does_not_dedupe_different_frames_from_same_side();
}
TEST(RelayTest, ReliableSenderDoesNotBlockWhileWaitingForAck) {
  test_relay_reliable_sender_does_not_block_while_waiting_for_ack();
}
TEST(RelayTest, TimeoutLimitsWorkPerCall) { test_relay_timeout_limits_work_per_call(); }
TEST(RelayTest, ConcurrentRxIsThreadSafe) { test_relay_concurrent_rx_is_thread_safe(); }
TEST(RelayTest, RouteControls) { test_relay_route_controls(); }
TEST(RelayTest, TypedRoutesCanTargetOneOrManySides) { test_relay_typed_routes_can_target_one_or_many_sides(); }
TEST(RelayTest, DiscoverySelectiveFanout) { test_relay_discovery_selective_fanout(); }
TEST(RelayTest, RemoveSideStopsTransmitAndRejectsRemovedIngress) {
  test_relay_remove_side_stops_transmit_and_rejects_removed_ingress();
}
TEST(RelayTest, RemoveSideUpdatesDiscoveryRoutesAndAnnouncesRemainingTopology) {
  test_relay_remove_side_updates_discovery_routes_and_announces_remaining_topology();
}
TEST(RelayTest, RuntimeRoutesSupportAsymmetricAndIngressOnlyLinks) {
  test_relay_runtime_routes_support_asymmetric_and_ingress_only_links();
}
TEST(RelayTest, WeightedRouteModeSplitsDiscoveredPathsByWeight) {
  test_relay_weighted_route_mode_splits_discovered_paths_by_weight();
}
TEST(RelayTest, FailoverRouteModeSwitchesWhenPreferredPathExpires) {
  test_relay_failover_route_mode_switches_when_preferred_path_expires();
}
TEST(RelayTest, WeightedRouteModeFallsBackToRemainingPathWhenOtherPathExpires) {
  test_relay_weighted_route_mode_falls_back_to_remaining_path_when_other_path_expires();
}
TEST(RelayTest, FailoverRouteModeSwitchesWhenPreferredSideIsRemoved) {
  test_relay_failover_route_mode_switches_when_preferred_side_is_removed();
}
TEST(RelayTest, PeriodicDispatchesDiscovery) { test_relay_periodic_dispatches_discovery(); }
TEST(RelayTest, DiscoverySenderIsRelay) { test_relay_discovery_sender_is_relay(); }
TEST(SystemTest, MultiBusFlow) { test_multibus_system_flow(); }
