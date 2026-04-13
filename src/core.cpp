#include "generated_schema.hpp"
#include "internal.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <tuple>

#ifdef SEDS_HAS_ZSTD
#include <zstd.h>
#endif

namespace seds {

const std::vector<TypeInfo> kTypeInfo = generated::make_type_info();
const uint32_t kEndpointCount = generated::kEndpointCountValue;
const std::vector<const char *> kEndpointNames = generated::make_endpoint_names();

size_t RouteKeyHash::operator()(const RouteKey &key) const noexcept {
  return (static_cast<size_t>(static_cast<uint32_t>(key.src_side)) << 32u) ^
         static_cast<size_t>(static_cast<uint32_t>(key.dst_side));
}

size_t TypedRouteKeyHash::operator()(const TypedRouteKey &key) const noexcept {
  return (static_cast<size_t>(static_cast<uint32_t>(key.src_side)) << 40u) ^ (static_cast<size_t>(key.ty) << 8u) ^
         static_cast<size_t>(static_cast<uint32_t>(key.dst_side));
}

uint64_t default_now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

bool valid_type(uint32_t ty) { return ty < kTypeInfo.size(); }
bool valid_endpoint(uint32_t ep) { return ep < kEndpointCount; }

uint32_t crc32_bytes(const uint8_t *data, size_t len) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      const uint32_t mask = -(crc & 1u);
      crc = (crc >> 1u) ^ (0xEDB88320u & mask);
    }
  }
  return ~crc;
}

uint64_t packet_id(const PacketData &pkt) {
  uint64_t h = 1469598103934665603ull;
  auto mix = [&](const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
      h ^= data[i];
      h *= 1099511628211ull;
    }
  };
  mix(reinterpret_cast<const uint8_t *>(pkt.sender.data()), pkt.sender.size());
  mix(reinterpret_cast<const uint8_t *>(&pkt.ty), sizeof(pkt.ty));
  for (uint32_t ep : pkt.endpoints) {
    mix(reinterpret_cast<const uint8_t *>(&ep), sizeof(ep));
  }
  mix(reinterpret_cast<const uint8_t *>(&pkt.timestamp), sizeof(pkt.timestamp));
  if (!pkt.payload.empty()) {
    mix(pkt.payload.data(), pkt.payload.size());
  }
  return h;
}

uint64_t sender_hash(const std::string_view sender) {
  uint64_t h = 0x517CC1B727220A95ull;
  for (const unsigned char ch : sender) {
    h ^= static_cast<uint64_t>(ch);
    h *= 1099511628211ull;
  }
  return h;
}

bool packet_from_view(const SedsPacketView *view, PacketData &out) {
  if (view == nullptr || !valid_type(view->ty) || view->endpoints == nullptr || view->num_endpoints == 0) {
    return false;
  }
  for (size_t i = 0; i < view->num_endpoints; ++i) {
    if (!valid_endpoint(view->endpoints[i])) {
      return false;
    }
  }
  if (view->payload_len != 0 && view->payload == nullptr) {
    return false;
  }
  out.ty = view->ty;
  out.sender.assign(view->sender ? view->sender : "", view->sender_len);
  out.endpoints.assign(view->endpoints, view->endpoints + view->num_endpoints);
  out.timestamp = view->timestamp;
  if (view->payload_len != 0) {
    out.payload.assign(view->payload, view->payload + view->payload_len);
  } else {
    out.payload.clear();
  }
  return true;
}

void fill_view(const PacketData &pkt, SedsPacketView &view) {
  view.ty = pkt.ty;
  view.data_size = pkt.payload.size();
  view.sender = pkt.sender.c_str();
  view.sender_len = pkt.sender.size();
  view.endpoints = pkt.endpoints.data();
  view.num_endpoints = pkt.endpoints.size();
  view.timestamp = pkt.timestamp;
  view.payload = pkt.payload.data();
  view.payload_len = pkt.payload.size();
}

bool validate_payload(uint32_t ty, size_t bytes) {
  if (!valid_type(ty))
    return false;
  const auto &info = kTypeInfo[ty];
  if (!info.dynamic) {
    return bytes == info.elem_size * info.static_count;
  }
  switch (info.data_type) {
  case ElementDataType::String:
  case ElementDataType::Binary:
    return true;
  case ElementDataType::NoData:
    return bytes == 0;
  default:
    return info.elem_size != 0 && (bytes % info.elem_size) == 0;
  }
}

std::string error_string(int32_t code) {
  switch (code) {
  case SEDS_OK:
    return "OK";
  case SEDS_ERR:
    return "ERR";
  case SEDS_GENERIC_ERROR:
    return "Generic error";
  case SEDS_INVALID_TYPE:
    return "Invalid type";
  case SEDS_SIZE_MISMATCH:
    return "Size mismatch";
  case SEDS_SIZE_MISMATCH_ERROR:
    return "Size mismatch error";
  case SEDS_EMPTY_ENDPOINTS:
    return "Empty endpoints";
  case SEDS_TIMESTAMP_INVALID:
    return "Timestamp invalid";
  case SEDS_MISSING_PAYLOAD:
    return "Missing payload";
  case SEDS_HANDLER_ERROR:
    return "Handler error";
  case SEDS_BAD_ARG:
    return "Bad arg";
  case SEDS_SERIALIZE:
    return "Serialize error";
  case SEDS_DESERIALIZE:
    return "Deserialize error";
  case SEDS_IO:
    return "I/O error";
  case SEDS_INVALID_UTF8:
    return "Invalid UTF-8";
  case SEDS_TYPE_MISMATCH:
    return "Type mismatch";
  case SEDS_INVALID_LINK_ID:
    return "Invalid link id";
  case SEDS_PACKET_TOO_LARGE:
    return "Packet too large";
  default:
    return "Unknown error";
  }
}

int32_t copy_text(std::string_view text, char *buf, size_t buf_len) {
  if (buf == nullptr || buf_len <= text.size()) {
    return SEDS_ERR;
  }
  std::memcpy(buf, text.data(), text.size());
  buf[text.size()] = '\0';
  return SEDS_OK;
}

PacketData make_internal_packet(uint32_t ty, uint64_t ts, std::vector<uint8_t> payload) {
  PacketData pkt;
  pkt.ty = ty;
  pkt.sender = "CPP";
  pkt.endpoints = kTypeInfo[ty].endpoints;
  pkt.timestamp = ts;
  pkt.payload = std::move(payload);
  return pkt;
}

PacketData make_reliable_control_packet(const uint32_t control_ty, const uint32_t ty, const uint32_t seq,
                                        const uint64_t ts, const std::string_view sender) {
  PacketData pkt;
  pkt.ty = control_ty;
  pkt.sender = std::string(sender);
  pkt.endpoints = kTypeInfo[control_ty].endpoints;
  pkt.timestamp = ts;
  append_le<uint32_t>(ty, pkt.payload);
  append_le<uint32_t>(seq, pkt.payload);
  return pkt;
}

PacketData make_e2e_reliable_ack_packet(const uint64_t packet_id, const uint64_t ts, const std::string_view sender) {
  PacketData pkt;
  pkt.ty = SEDS_DT_RELIABLE_ACK;
  pkt.sender = std::string(sender);
  pkt.endpoints = kTypeInfo[SEDS_DT_RELIABLE_ACK].endpoints;
  pkt.timestamp = ts;
  append_le<uint64_t>(packet_id, pkt.payload);
  return pkt;
}

std::vector<uint32_t> local_endpoints_for_error(const std::vector<LocalEndpoint> &locals) {
  std::vector<uint32_t> endpoints;
  endpoints.reserve(locals.size());
  for (const auto &local : locals) {
    if (std::ranges::find(endpoints, local.endpoint) == endpoints.end()) {
      endpoints.push_back(local.endpoint);
    }
  }
  if (endpoints.empty() && valid_type(SEDS_DT_TELEMETRY_ERROR)) {
    endpoints = kTypeInfo[SEDS_DT_TELEMETRY_ERROR].endpoints;
  }
  return endpoints;
}

PacketData make_router_error_packet(const std::vector<LocalEndpoint> &locals, std::string message) {
  return make_router_error_packet(local_endpoints_for_error(locals), std::move(message));
}

PacketData make_router_error_packet(std::vector<uint32_t> endpoints, std::string message) {
  constexpr size_t kMaxErrorPayloadBytes = 1024;
  if (message.size() > kMaxErrorPayloadBytes) {
    message.resize(kMaxErrorPayloadBytes);
  }
  PacketData pkt;
  pkt.ty = SEDS_DT_TELEMETRY_ERROR;
  pkt.sender = "CPP";
  pkt.endpoints = std::move(endpoints);
  pkt.timestamp = 0;
  pkt.payload.assign(message.begin(), message.end());
  return pkt;
}

std::vector<int32_t> apply_policy(RoutePolicy &policy, std::vector<int32_t> sides) {
  if (sides.empty() || policy.mode == Seds_RSM_Fanout) {
    return sides;
  }
  if (policy.mode == Seds_RSM_Failover) {
    int32_t best = sides.front();
    uint32_t best_pri = UINT32_MAX;
    for (int32_t side : sides) {
      const auto it = policy.priorities.find(side);
      const uint32_t pri = it == policy.priorities.end() ? UINT32_MAX / 2u : it->second;
      if (pri < best_pri) {
        best_pri = pri;
        best = side;
      }
    }
    return {best};
  }
  uint64_t total = 0;
  for (int32_t side : sides) {
    const auto it = policy.weights.find(side);
    total += it == policy.weights.end() ? 1u : std::max<uint32_t>(1u, it->second);
  }
  if (total == 0) {
    return {sides.front()};
  }
  const uint64_t pick = policy.rr_counter++ % total;
  uint64_t running = 0;
  for (int32_t side : sides) {
    const auto it = policy.weights.find(side);
    running += it == policy.weights.end() ? 1u : std::max<uint32_t>(1u, it->second);
    if (pick < running) {
      return {side};
    }
  }
  return {sides.front()};
}

uint64_t reliable_key(int32_t side_id, uint32_t ty) {
  return (static_cast<uint64_t>(static_cast<uint32_t>(side_id)) << 32u) | ty;
}

bool is_reliable_type(uint32_t ty) { return valid_type(ty) && kTypeInfo[ty].reliable(); }

bool endpoint_link_local_only(uint32_t ep) {
  if (!valid_endpoint(ep)) {
    return false;
  }
  bool saw_match = false;
  for (const auto &info : kTypeInfo) {
    if (std::ranges::find(info.endpoints, ep) == info.endpoints.end()) {
      continue;
    }
    saw_match = true;
    if (!info.link_local_only) {
      return false;
    }
  }
  return saw_match;
}

bool packet_requires_link_local(const PacketData &pkt) {
  if (pkt.endpoints.empty()) {
    return false;
  }
  return std::ranges::all_of(pkt.endpoints, endpoint_link_local_only);
}

bool side_accepts_packet(const Side &side, const PacketData &pkt) {
  if (packet_requires_link_local(pkt) && !side.link_local_enabled) {
    return false;
  }
  return true;
}

namespace {

std::tuple<int32_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint32_t> split_unix_ms(uint64_t unix_ms) {
  const uint64_t total_seconds = unix_ms / 1000u;
  const auto millisecond = static_cast<uint32_t>(unix_ms % 1000u);
  const auto days = static_cast<int64_t>(total_seconds / 86400u);
  const auto sod = static_cast<uint32_t>(total_seconds % 86400u);

  int64_t z = days + 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const auto doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int32_t year = static_cast<int32_t>(yoe) + static_cast<int32_t>(era) * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const auto day = static_cast<uint8_t>(doy - (153 * mp + 2) / 5 + 1);
  const auto month = static_cast<uint8_t>(mp + (mp < 10 ? 3 : -9));
  year += month <= 2 ? 1 : 0;

  const auto hour = static_cast<uint8_t>(sod / 3600u);
  const auto minute = static_cast<uint8_t>((sod % 3600u) / 60u);
  const auto second = static_cast<uint8_t>(sod % 60u);
  return {year, month, day, hour, minute, second, millisecond * 1000000u};
}

constexpr bool is_leap_year(int32_t year) { return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0); }

constexpr uint8_t days_in_month(int32_t year, uint8_t month) {
  switch (month) {
  case 1:
  case 3:
  case 5:
  case 7:
  case 8:
  case 10:
  case 12:
    return 31;
  case 4:
  case 6:
  case 9:
  case 11:
    return 30;
  case 2:
    return static_cast<uint8_t>(is_leap_year(year) ? 29 : 28);
  default:
    return 0;
  }
}

constexpr int64_t days_from_civil(int32_t year, uint8_t month, uint8_t day) {
  year -= month <= 2 ? 1 : 0;
  const int32_t era = (year >= 0 ? year : year - 399) / 400;
  const auto yoe = static_cast<unsigned>(year - era * 400);
  const unsigned month_index = month > 2 ? static_cast<unsigned>(month - 3) : static_cast<unsigned>(month + 9);
  const unsigned doy = (153u * month_index + 2u) / 5u + day - 1u;
  const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
}

} // namespace

void fill_network_time(uint64_t unix_ms, SedsNetworkTime &out) {
  std::memset(&out, 0, sizeof(out));
  const auto [year, month, day, hour, minute, second, nanosecond] = split_unix_ms(unix_ms);
  out.has_unix_time_ms = true;
  out.unix_time_ms = unix_ms;
  out.has_year = true;
  out.year = year;
  out.has_month = true;
  out.month = month;
  out.has_day = true;
  out.day = day;
  out.has_hour = true;
  out.hour = hour;
  out.has_minute = true;
  out.minute = minute;
  out.has_second = true;
  out.second = second;
  out.has_nanosecond = true;
  out.nanosecond = nanosecond;
}

std::optional<uint64_t> network_time_to_unix_ms(int32_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute,
                                                uint8_t second, uint32_t nanosecond) {
  if (month < 1 || month > 12 || day < 1 || day > days_in_month(year, month) || hour > 23 || minute > 59 ||
      second > 59 || nanosecond >= 1000000000u) {
    return std::nullopt;
  }
  const int64_t days = days_from_civil(year, month, day);
  const uint64_t seconds = static_cast<uint64_t>(days) * 86400u + static_cast<uint64_t>(hour) * 3600u +
                           static_cast<uint64_t>(minute) * 60u + static_cast<uint64_t>(second);
  return seconds * 1000u + nanosecond / 1000000u;
}

} // namespace seds

SedsRouter::SedsRouter(SedsRouterMode router_mode) : mode(router_mode) {
  sender = "CPP";
  node_sender = sender;
}

uint64_t SedsRouter::now_ms() const { return now_ms_cb ? now_ms_cb(clock_user) : seds::default_now_ms(); }

uint64_t SedsRouter::current_network_ms() const {
  if (!timesync.has_network_time) {
    return 0;
  }
  return timesync.network_anchor_unix_ms + (now_ms() - timesync.network_anchor_local_ms);
}

uint64_t SedsRelay::now_ms() const { return now_ms_cb ? now_ms_cb(clock_user) : seds::default_now_ms(); }
