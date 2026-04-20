#include "sedsprintf.h"

#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct ManualClock {
  uint64_t now_ms{};
};

struct TxCapture {
  std::vector<std::vector<uint8_t>> frames;
};

struct RxCapture {
  bool seen{};
  float values[3]{};
};

uint64_t read_clock(void *user) {
  return static_cast<ManualClock *>(user)->now_ms;
}

SedsResult capture_tx(const uint8_t *bytes, size_t len, void *user) {
  auto *capture = static_cast<TxCapture *>(user);
  capture->frames.emplace_back(bytes, bytes + len);
  return SEDS_OK;
}

SedsResult capture_packet(const SedsPacketView *pkt, void *user) {
  auto *capture = static_cast<RxCapture *>(user);
  if (pkt == nullptr || pkt->ty != SEDS_DT_GPS_DATA || pkt->payload_len != sizeof(capture->values)) {
    return SEDS_ERR;
  }
  std::memcpy(capture->values, pkt->payload, sizeof(capture->values));
  capture->seen = true;
  return SEDS_OK;
}

std::string hex_encode(const std::vector<uint8_t> &bytes) {
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (const uint8_t byte : bytes) {
    out << std::setw(2) << static_cast<unsigned int>(byte);
  }
  return out.str();
}

int hex_value(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

bool hex_decode(const std::string &hex, std::vector<uint8_t> &out) {
  if (hex.size() % 2 != 0) {
    return false;
  }
  out.clear();
  out.reserve(hex.size() / 2);
  for (size_t i = 0; i < hex.size(); i += 2) {
    const int hi = hex_value(hex[i]);
    const int lo = hex_value(hex[i + 1]);
    if (hi < 0 || lo < 0) {
      return false;
    }
    out.push_back(static_cast<uint8_t>((hi << 4) | lo));
  }
  return true;
}

bool decode_arg(const char *arg, std::vector<uint8_t> &out) {
  if (!hex_decode(arg, out)) {
    std::cerr << "invalid hex input\n";
    return false;
  }
  return true;
}

bool is_link_ack_for_gps(const std::vector<uint8_t> &frame) {
  SedsOwnedPacket *owned = seds_pkt_deserialize_owned(frame.data(), frame.size());
  if (owned == nullptr) {
    return false;
  }
  SedsPacketView view{};
  const bool ok = seds_owned_pkt_view(owned, &view) == SEDS_OK;
  bool matches = false;
  if (ok && view.ty == SEDS_DT_RELIABLE_ACK && view.payload_len == sizeof(uint32_t) * 2u &&
      view.sender != nullptr) {
    uint32_t ack_ty = 0;
    std::memcpy(&ack_ty, view.payload, sizeof(ack_ty));
    const std::string sender(view.sender, view.sender_len);
    matches = ack_ty == SEDS_DT_GPS_DATA && sender.rfind("E2EACK:", 0) != 0;
  }
  seds_owned_pkt_free(owned);
  return matches;
}

const std::vector<uint8_t> *find_link_ack_for_gps(const TxCapture &tx) {
  for (const auto &frame : tx.frames) {
    if (is_link_ack_for_gps(frame)) {
      return &frame;
    }
  }
  return nullptr;
}

int emit_frame() {
  TxCapture tx;
  SedsRouter *router = seds_router_new(Seds_RM_Sink, nullptr, nullptr, nullptr, 0);
  if (router == nullptr) {
    return 2;
  }
  const int32_t side = seds_router_add_side_serialized(router, "rust", 4, capture_tx, &tx, false);
  const float values[3] = {41.0f, 42.5f, -7.25f};
  const SedsResult rc = seds_router_log_f32(router, SEDS_DT_GPS_DATA, values, 3);
  seds_router_free(router);
  if (side < 0 || rc != SEDS_OK || tx.frames.empty()) {
    return 2;
  }
  std::cout << hex_encode(tx.frames.front()) << "\n";
  return 0;
}

SedsRouter *make_receive_router(RxCapture &rx, ManualClock *clock = nullptr) {
  const SedsLocalEndpointDesc handlers[] = {
      {.endpoint = SEDS_EP_RADIO, .packet_handler = capture_packet, .serialized_handler = nullptr, .user = &rx},
  };
  return seds_router_new(Seds_RM_Sink, clock == nullptr ? nullptr : read_clock, clock, handlers, 1);
}

int consume_frame(const std::string &hex) {
  std::vector<uint8_t> bytes;
  if (!hex_decode(hex, bytes)) {
    return 2;
  }
  RxCapture rx;
  SedsRouter *router = make_receive_router(rx);
  const SedsResult rc = seds_router_receive_serialized(router, bytes.data(), bytes.size());
  seds_router_free(router);
  if (rc != SEDS_OK || !rx.seen) {
    return 2;
  }
  std::cout << rx.values[0] << " " << rx.values[1] << " " << rx.values[2] << "\n";
  return 0;
}

int consume_reliable(const std::string &hex) {
  std::vector<uint8_t> bytes;
  if (!hex_decode(hex, bytes)) {
    return 2;
  }
  ManualClock clock{123};
  TxCapture tx;
  RxCapture rx;
  SedsRouter *router = make_receive_router(rx, &clock);
  const int32_t side = seds_router_add_side_serialized(router, "peer", 4, capture_tx, &tx, true);
  const SedsResult rc =
      seds_router_receive_serialized_from_side(router, static_cast<uint32_t>(side), bytes.data(), bytes.size());
  const SedsResult tx_rc = seds_router_process_tx_queue(router);
  seds_router_free(router);
  const std::vector<uint8_t> *ack = find_link_ack_for_gps(tx);
  if (side < 0 || rc != SEDS_OK || tx_rc != SEDS_OK || !rx.seen || ack == nullptr) {
    return 2;
  }
  std::cout << hex_encode(*ack) << "\n";
  for (const auto &frame : tx.frames) {
    if (&frame != ack) {
      std::cout << hex_encode(frame) << "\n";
    }
  }
  std::cout << rx.values[0] << " " << rx.values[1] << " " << rx.values[2] << "\n";
  return 0;
}

int reliable_session() {
  ManualClock clock{123};
  TxCapture tx;
  SedsRouter *router = seds_router_new(Seds_RM_Sink, read_clock, &clock, nullptr, 0);
  const int32_t side = seds_router_add_side_serialized(router, "peer", 4, capture_tx, &tx, true);
  const float values[3] = {51.0f, 52.0f, 53.0f};
  if (side < 0 || seds_router_log_f32(router, SEDS_DT_GPS_DATA, values, 3) != SEDS_OK || tx.frames.empty()) {
    seds_router_free(router);
    return 2;
  }
  std::cout << hex_encode(tx.frames.back()) << "\n" << std::flush;

  bool saw_ack = false;
  std::string ack_hex;
  while (std::getline(std::cin, ack_hex)) {
    if (ack_hex.empty()) {
      continue;
    }
    std::vector<uint8_t> ack;
    if (!hex_decode(ack_hex, ack) ||
        seds_router_receive_serialized_from_side(router, static_cast<uint32_t>(side), ack.data(), ack.size()) !=
            SEDS_OK) {
      seds_router_free(router);
      return 2;
    }
    saw_ack = true;
  }
  if (!saw_ack) {
    seds_router_free(router);
    return 2;
  }
  tx.frames.clear();
  clock.now_ms = 500;
  if (seds_router_process_tx_queue(router) != SEDS_OK || !tx.frames.empty()) {
    seds_router_free(router);
    return 2;
  }
  seds_router_free(router);
  std::cout << "ACK_ACCEPTED\n";
  return 0;
}

int emit_discovery() {
  TxCapture tx;
  RxCapture rx;
  SedsRouter *router = make_receive_router(rx);
  seds_router_set_sender(router, "CPP_DISC", 8);
  const int32_t side = seds_router_add_side_serialized(router, "rust", 4, capture_tx, &tx, false);
  if (side < 0 || seds_router_announce_discovery(router) != SEDS_OK ||
      seds_router_process_tx_queue(router) != SEDS_OK || tx.frames.empty()) {
    seds_router_free(router);
    return 2;
  }
  for (const auto &frame : tx.frames) {
    std::cout << hex_encode(frame) << "\n";
  }
  seds_router_free(router);
  return 0;
}

int consume_discovery(int argc, char **argv) {
  SedsRouter *router = seds_router_new(Seds_RM_Sink, nullptr, nullptr, nullptr, 0);
  const int32_t side = seds_router_add_side_serialized(router, "peer", 4, capture_tx, nullptr, false);
  if (side < 0) {
    seds_router_free(router);
    return 2;
  }
  for (int i = 2; i < argc; ++i) {
    std::vector<uint8_t> bytes;
    if (!decode_arg(argv[i], bytes) ||
        seds_router_rx_serialized_packet_to_queue_from_side(router, static_cast<uint32_t>(side), bytes.data(),
                                                            bytes.size()) != SEDS_OK) {
      seds_router_free(router);
      return 2;
    }
  }
  if (seds_router_process_all_queues(router) != SEDS_OK) {
    seds_router_free(router);
    return 2;
  }
  const int32_t len = seds_router_export_topology_len(router);
  std::string json(static_cast<size_t>(len), '\0');
  if (len <= 0 || seds_router_export_topology(router, json.data(), json.size()) != SEDS_OK ||
      json.find("\"reachable_endpoints\":[1") == std::string::npos) {
    seds_router_free(router);
    return 2;
  }
  seds_router_free(router);
  std::cout << "DISCOVERY_OK\n";
  return 0;
}

int emit_timesync() {
  ManualClock clock{1000};
  TxCapture tx;
  SedsRouter *router = seds_router_new(Seds_RM_Sink, read_clock, &clock, nullptr, 0);
  seds_router_set_sender(router, "CPP_TIME", 8);
  const int32_t side = seds_router_add_side_serialized(router, "peer", 4, capture_tx, &tx, false);
  if (side < 0 ||
      seds_router_configure_timesync(router, true, 1, 10, 5000, 1000, 1000) != SEDS_OK ||
      seds_router_set_local_network_datetime_millis(router, 2026, 1, 2, 3, 4, 5, 0) != SEDS_OK ||
      seds_router_poll_timesync(router, nullptr) != SEDS_OK || seds_router_process_tx_queue(router) != SEDS_OK ||
      tx.frames.empty()) {
    seds_router_free(router);
    return 2;
  }
  for (const auto &frame : tx.frames) {
    std::cout << hex_encode(frame) << "\n";
  }
  seds_router_free(router);
  return 0;
}

int consume_timesync(int argc, char **argv) {
  ManualClock clock{1000};
  SedsRouter *router = seds_router_new(Seds_RM_Sink, read_clock, &clock, nullptr, 0);
  const int32_t side = seds_router_add_side_serialized(router, "peer", 4, capture_tx, nullptr, false);
  if (side < 0 || seds_router_configure_timesync(router, true, 0, 100, 5000, 1000, 1000) != SEDS_OK) {
    seds_router_free(router);
    return 2;
  }
  for (int i = 2; i < argc; ++i) {
    std::vector<uint8_t> bytes;
    if (!decode_arg(argv[i], bytes) ||
        seds_router_receive_serialized_from_side(router, static_cast<uint32_t>(side), bytes.data(), bytes.size()) !=
            SEDS_OK) {
      seds_router_free(router);
      return 2;
    }
  }
  uint64_t network_ms = 0;
  if (seds_router_get_network_time_ms(router, &network_ms) != SEDS_OK || network_ms == 0) {
    seds_router_free(router);
    return 2;
  }
  seds_router_free(router);
  std::cout << network_ms << "\n";
  return 0;
}

void print_relay_frames(const char *prefix, const TxCapture &capture) {
  for (const auto &frame : capture.frames) {
    std::cout << prefix << " " << hex_encode(frame) << "\n";
  }
}

int relay_session() {
  ManualClock clock{123};
  TxCapture to_source;
  TxCapture to_dest;
  SedsRelay *relay = seds_relay_new(read_clock, &clock);
  const int32_t source =
      seds_relay_add_side_serialized(relay, "source", 6, capture_tx, &to_source, true);
  const int32_t dest = seds_relay_add_side_serialized(relay, "dest", 4, capture_tx, &to_dest, true);
  if (source < 0 || dest < 0) {
    seds_relay_free(relay);
    return 2;
  }

  std::string data_hex;
  if (!std::getline(std::cin, data_hex)) {
    seds_relay_free(relay);
    return 2;
  }
  std::vector<uint8_t> data;
  if (!hex_decode(data_hex, data) ||
      seds_relay_rx_serialized_from_side(relay, static_cast<uint32_t>(source), data.data(), data.size()) != SEDS_OK ||
      seds_relay_process_all_queues(relay) != SEDS_OK || to_source.frames.empty() || to_dest.frames.empty()) {
    seds_relay_free(relay);
    return 2;
  }
  print_relay_frames("SRC", to_source);
  print_relay_frames("DST", to_dest);
  std::cout << "END\n" << std::flush;
  to_source.frames.clear();
  to_dest.frames.clear();

  std::string ack_hex;
  while (std::getline(std::cin, ack_hex)) {
    if (ack_hex.empty()) {
      continue;
    }
    std::vector<uint8_t> ack;
    if (!hex_decode(ack_hex, ack) ||
        seds_relay_rx_serialized_from_side(relay, static_cast<uint32_t>(dest), ack.data(), ack.size()) != SEDS_OK ||
        seds_relay_process_all_queues(relay) != SEDS_OK) {
      seds_relay_free(relay);
      return 2;
    }
  }
  if (to_source.frames.empty()) {
    seds_relay_free(relay);
    return 2;
  }
  print_relay_frames("SRC", to_source);
  print_relay_frames("DST", to_dest);
  std::cout << "END\n";
  seds_relay_free(relay);
  return 0;
}

int relay_forward(int argc, char **argv) {
  TxCapture to_dest;
  SedsRelay *relay = seds_relay_new(nullptr, nullptr);
  const int32_t source = seds_relay_add_side_serialized(relay, "source", 6, capture_tx, nullptr, false);
  const int32_t dest = seds_relay_add_side_serialized(relay, "dest", 4, capture_tx, &to_dest, false);
  if (source < 0 || dest < 0) {
    seds_relay_free(relay);
    return 2;
  }
  for (int i = 2; i < argc; ++i) {
    std::vector<uint8_t> bytes;
    if (!decode_arg(argv[i], bytes) ||
        seds_relay_rx_serialized_from_side(relay, static_cast<uint32_t>(source), bytes.data(), bytes.size()) !=
            SEDS_OK ||
        seds_relay_process_all_queues(relay) != SEDS_OK) {
      seds_relay_free(relay);
      return 2;
    }
  }
  if (to_dest.frames.empty()) {
    seds_relay_free(relay);
    return 2;
  }
  for (const auto &frame : to_dest.frames) {
    std::cout << hex_encode(frame) << "\n";
  }
  seds_relay_free(relay);
  return 0;
}

}  // namespace

int main(int argc, char **argv) {
  const std::string cmd = argc >= 2 ? argv[1] : "";
  if (argc == 2 && cmd == "emit") {
    return emit_frame();
  }
  if (argc == 3 && cmd == "consume") {
    return consume_frame(argv[2]);
  }
  if (argc == 3 && cmd == "consume-reliable") {
    return consume_reliable(argv[2]);
  }
  if (argc == 2 && cmd == "reliable-session") {
    return reliable_session();
  }
  if (argc == 2 && cmd == "emit-discovery") {
    return emit_discovery();
  }
  if (argc >= 3 && cmd == "consume-discovery") {
    return consume_discovery(argc, argv);
  }
  if (argc == 2 && cmd == "emit-timesync") {
    return emit_timesync();
  }
  if (argc >= 3 && cmd == "consume-timesync") {
    return consume_timesync(argc, argv);
  }
  if (argc == 2 && cmd == "relay-session") {
    return relay_session();
  }
  if (argc >= 3 && cmd == "relay-forward") {
    return relay_forward(argc, argv);
  }
  std::cerr << "usage: rust_interop_cpp_peer <command>\n";
  return 2;
}
