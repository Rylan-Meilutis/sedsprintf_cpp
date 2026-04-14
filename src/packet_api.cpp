#include "internal.hpp"

#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
#include <tuple>
#include <type_traits>

namespace seds
{
    namespace
    {
        void json_push_escaped(std::string & out, std::string_view value)
        {
            out.push_back('"');
            for (const unsigned char ch: value)
            {
                switch (ch)
                {
                    case '"':
                        out += "\\\"";
                        break;
                    case '\\':
                        out += "\\\\";
                        break;
                    case '\b':
                        out += "\\b";
                        break;
                    case '\f':
                        out += "\\f";
                        break;
                    case '\n':
                        out += "\\n";
                        break;
                    case '\r':
                        out += "\\r";
                        break;
                    case '\t':
                        out += "\\t";
                        break;
                    default:
                        if (ch < 0x20u)
                        {
                            std::ostringstream os;
                            os << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<unsigned>(ch);
                            out += os.str();
                        }
                        else
                        {
                            out.push_back(static_cast<char>(ch));
                        }
                }
            }
            out.push_back('"');
        }

        constexpr uint64_t kEpochMsThreshold = 1'000'000'000'000ull;
        constexpr int kStringPrecision = 8;

        template<typename T>
        T read_le_value(const uint8_t * data)
        {
            T value{};
            std::memcpy(&value, data, sizeof(T));
            return value;
        }

        std::string format_epoch_ms(uint64_t total_ms)
        {
            auto div_mod = [](uint64_t n, uint64_t d) -> std::pair<uint64_t, uint64_t>
            {
                return {n / d, n % d};
            };
            auto civil_from_days = [](int64_t z) -> std::tuple<int32_t, uint32_t, uint32_t>
            {
                z += 719468;
                const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
                const int64_t doe = z - era * 146097;
                const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
                const auto y = static_cast<int32_t>(yoe) + static_cast<int32_t>(era) * 400;
                const auto doy = static_cast<int32_t>(doe - (365 * yoe + yoe / 4 - yoe / 100));
                const auto mp = (5 * doy + 2) / 153;
                const auto d = doy - (153 * mp + 2) / 5 + 1;
                const auto m = mp + (mp < 10 ? 3 : -9);
                return {y + static_cast<int32_t>(m <= 2), static_cast<uint32_t>(m), static_cast<uint32_t>(d)};
            };

            const auto [secs, sub_ms] = div_mod(total_ms, 1000);
            const auto days = static_cast<int64_t>(secs / 86400);
            const auto sod = static_cast<uint32_t>(secs % 86400);
            const auto [year, month, day] = civil_from_days(days);
            const auto hour = sod / 3600;
            const auto min = (sod % 3600) / 60;
            const auto sec = sod % 60;
            std::ostringstream os;
            os << std::setfill('0')
                    << std::setw(4) << year << "-"
                    << std::setw(2) << month << "-"
                    << std::setw(2) << day << " "
                    << std::setw(2) << hour << ":"
                    << std::setw(2) << min << ":"
                    << std::setw(2) << sec << "."
                    << std::setw(3) << sub_ms << "Z";
            return os.str();
        }

        std::string human_time(uint64_t total_ms)
        {
            if (total_ms >= kEpochMsThreshold)
            {
                return format_epoch_ms(total_ms);
            }
            const auto hours = total_ms / 3600000;
            const auto minutes = (total_ms % 3600000) / 60000;
            const auto seconds = (total_ms % 60000) / 1000;
            const auto milliseconds = total_ms % 1000;
            std::ostringstream os;
            if (hours > 0)
            {
                os << hours << "h " << std::setw(2) << std::setfill('0') << minutes
                        << "m " << std::setw(2) << seconds << "s " << std::setw(3) << milliseconds << "ms";
            }
            else if (minutes > 0)
            {
                os << minutes << "m " << std::setw(2) << std::setfill('0') << seconds
                        << "s " << std::setw(3) << milliseconds << "ms";
            }
            else
            {
                os << seconds << "s " << std::setw(3) << std::setfill('0') << milliseconds << "ms";
            }
            return os.str();
        }

        std::string endpoint_name(uint32_t ep)
        {
            if (ep < kEndpointNames.size())
            {
                return kEndpointNames[ep];
            }
            return "EP_" + std::to_string(ep);
        }

        template<typename T>
        void append_numbers(std::ostringstream & os, const uint8_t * data, size_t len)
        {
            bool first = true;
            for (size_t off = 0; off + sizeof(T) <= len; off += sizeof(T))
            {
                if (!first)
                {
                    os << ", ";
                }
                first = false;
                if constexpr (std::is_same_v<T, float> || std::is_same_v<T, double>)
                {
                    os << std::fixed << std::setprecision(kStringPrecision) << read_le_value<T>(data + off);
                }
                else if constexpr (std::is_same_v<T, uint8_t> || std::is_same_v<T, int8_t>)
                {
                    os << static_cast<int>(read_le_value<T>(data + off));
                }
                else
                {
                    os << read_le_value<T>(data + off);
                }
            }
        }

        std::string hex_dump(const uint8_t * data, size_t len)
        {
            std::ostringstream os;
            for (size_t i = 0; i < len; ++i)
            {
                if (i != 0)
                {
                    os << ' ';
                }
                os << "0x" << std::hex << std::setfill('0') << std::setw(2) << static_cast<unsigned>(data[i]) <<
                        std::dec;
            }
            return os.str();
        }

        std::string trimmed_utf8(const SedsPacketView & pkt)
        {
            size_t end = pkt.payload_len;
            while (end > 0 && pkt.payload[end - 1] == 0)
            {
                --end;
            }
            return {reinterpret_cast<const char *>(pkt.payload), end};
        }

        std::string payload_to_string(const SedsPacketView & pkt)
        {
            const auto & info = kTypeInfo[pkt.ty];
            if (pkt.payload_len == 0)
            {
                return "<NoData>";
            }

            std::ostringstream os;
            switch (info.data_type)
            {
                case ElementDataType::String:
                    os << '"' << trimmed_utf8(pkt) << '"';
                    break;
                case ElementDataType::Binary:
                    return "Data (hex): " + hex_dump(pkt.payload, pkt.payload_len);
                case ElementDataType::Bool:
                {
                    for (size_t i = 0; i < pkt.payload_len; ++i)
                    {
                        if (i != 0)
                        {
                            os << ", ";
                        }
                        os << (pkt.payload[i] != 0 ? "true" : "false");
                    }
                    break;
                }
                case ElementDataType::UInt8:
                    append_numbers<uint8_t>(os, pkt.payload, pkt.payload_len);
                    break;
                case ElementDataType::UInt16:
                    append_numbers<uint16_t>(os, pkt.payload, pkt.payload_len);
                    break;
                case ElementDataType::UInt32:
                    append_numbers<uint32_t>(os, pkt.payload, pkt.payload_len);
                    break;
                case ElementDataType::UInt64:
                    append_numbers<uint64_t>(os, pkt.payload, pkt.payload_len);
                    break;
                case ElementDataType::Int8:
                    append_numbers<int8_t>(os, pkt.payload, pkt.payload_len);
                    break;
                case ElementDataType::Int16:
                    append_numbers<int16_t>(os, pkt.payload, pkt.payload_len);
                    break;
                case ElementDataType::Int32:
                    append_numbers<int32_t>(os, pkt.payload, pkt.payload_len);
                    break;
                case ElementDataType::Int64:
                    append_numbers<int64_t>(os, pkt.payload, pkt.payload_len);
                    break;
                case ElementDataType::Float32:
                    append_numbers<float>(os, pkt.payload, pkt.payload_len);
                    break;
                case ElementDataType::Float64:
                    append_numbers<double>(os, pkt.payload, pkt.payload_len);
                    break;
                case ElementDataType::NoData:
                    os << "<NoData>";
                    break;
                case ElementDataType::UInt128:
                case ElementDataType::Int128:
                    os << hex_dump(pkt.payload, pkt.payload_len);
                    break;
            }
            return os.str();
        }
    } // namespace

    std::string topology_snapshot_to_json(const TopologySnapshot & snap)
    {
        const auto push_u32_array = [](std::string & out, const std::vector<uint32_t> & vals)
        {
            out.push_back('[');
            for (size_t i = 0; i < vals.size(); ++i)
            {
                if (i != 0)
                {
                    out.push_back(',');
                }
                out += std::to_string(vals[i]);
            }
            out.push_back(']');
        };
        const auto push_string_array = [](std::string & out, const std::vector<std::string> & vals)
        {
            out.push_back('[');
            for (size_t i = 0; i < vals.size(); ++i)
            {
                if (i != 0)
                {
                    out.push_back(',');
                }
                json_push_escaped(out, vals[i]);
            }
            out.push_back(']');
        };
        const auto push_board = [&](std::string & out, const TopologyBoardNode & board)
        {
            out += "{\"sender_id\":";
            json_push_escaped(out, board.sender_id);
            out += ",\"reachable_endpoints\":";
            push_u32_array(out, board.reachable_endpoints);
            out += ",\"reachable_timesync_sources\":";
            push_string_array(out, board.reachable_timesync_sources);
            out += ",\"connections\":";
            push_string_array(out, board.connections);
            out.push_back('}');
        };

        std::string out;
        out += "{\"advertised_endpoints\":";
        push_u32_array(out, snap.advertised_endpoints);
        out += ",\"advertised_timesync_sources\":";
        push_string_array(out, snap.advertised_timesync_sources);
        out += ",\"routers\":[";
        for (size_t i = 0; i < snap.routers.size(); ++i)
        {
            if (i != 0)
            {
                out.push_back(',');
            }
            push_board(out, snap.routers[i]);
        }
        out += "],\"routes\":[";
        for (size_t route_i = 0; route_i < snap.routes.size(); ++route_i)
        {
            const auto & route = snap.routes[route_i];
            if (route_i != 0)
            {
                out.push_back(',');
            }
            out += "{\"side_id\":";
            out += std::to_string(route.side_id);
            out += ",\"side_name\":";
            json_push_escaped(out, route.side_name);
            out += ",\"reachable_endpoints\":";
            push_u32_array(out, route.reachable_endpoints);
            out += ",\"reachable_timesync_sources\":";
            push_string_array(out, route.reachable_timesync_sources);
            out += ",\"announcers\":[";
            for (size_t ann_i = 0; ann_i < route.announcers.size(); ++ann_i)
            {
                const auto & announcer = route.announcers[ann_i];
                if (ann_i != 0)
                {
                    out.push_back(',');
                }
                out += "{\"sender_id\":";
                json_push_escaped(out, announcer.sender_id);
                out += ",\"reachable_endpoints\":";
                push_u32_array(out, announcer.reachable_endpoints);
                out += ",\"reachable_timesync_sources\":";
                push_string_array(out, announcer.reachable_timesync_sources);
                out += ",\"routers\":[";
                for (size_t board_i = 0; board_i < announcer.routers.size(); ++board_i)
                {
                    if (board_i != 0)
                    {
                        out.push_back(',');
                    }
                    push_board(out, announcer.routers[board_i]);
                }
                out += "],\"last_seen_ms\":";
                out += std::to_string(announcer.last_seen_ms);
                out += ",\"age_ms\":";
                out += std::to_string(announcer.age_ms);
                out.push_back('}');
            }
            out += "],\"last_seen_ms\":";
            out += std::to_string(route.last_seen_ms);
            out += ",\"age_ms\":";
            out += std::to_string(route.age_ms);
            out.push_back('}');
        }
        out += "],\"current_announce_interval_ms\":";
        out += std::to_string(snap.current_announce_interval_ms);
        out += ",\"next_announce_ms\":";
        out += std::to_string(snap.next_announce_ms);
        out.push_back('}');
        return out;
    }

    std::string packet_header_string(const SedsPacketView & pkt)
    {
        std::ostringstream os;
        os << "Type: " << kTypeInfo[pkt.ty].name
                << ", Data Size: " << pkt.payload_len
                << ", Sender: " << std::string(pkt.sender ? pkt.sender : "", pkt.sender_len)
                << ", Endpoints: [";
        for (size_t i = 0; i < pkt.num_endpoints; ++i)
        {
            if (i != 0)
            {
                os << ", ";
            }
            os << endpoint_name(pkt.endpoints[i]);
        }
        os << "], Timestamp: " << pkt.timestamp << " (" << human_time(pkt.timestamp) << ")";
        return os.str();
    }

    std::string packet_to_string(const SedsPacketView & pkt)
    {
        const auto & info = kTypeInfo[pkt.ty];
        if (info.data_type == ElementDataType::Binary && pkt.payload_len != 0)
        {
            return packet_header_string(pkt) + ", " + payload_to_string(pkt);
        }
        std::string out = "{";
        out += packet_header_string(pkt);
        out += ", ";
        switch (info.message_class)
        {
            case MessageClass::Data:
                out += "Data: (";
                break;
            case MessageClass::Error:
                out += "Error: (";
                break;
            case MessageClass::Warning:
                out += "Warning: (";
                break;
        }
        out += payload_to_string(pkt);
        out += ")}";
        return out;
    }
} // namespace seds

extern "C" {
SedsResult seds_pkt_to_string(const SedsPacketView * pkt, char * buf, size_t buf_len)
{
    if (pkt == nullptr || buf == nullptr || !seds::valid_type(pkt->ty))
    {
        return SEDS_BAD_ARG;
    }
    return static_cast<SedsResult>(seds::copy_text(seds::packet_to_string(*pkt), buf, buf_len));
}

int32_t seds_pkt_to_string_len(const SedsPacketView * pkt)
{
    if (pkt == nullptr || !seds::valid_type(pkt->ty))
    {
        return SEDS_BAD_ARG;
    }
    return static_cast<int32_t>(seds::packet_to_string(*pkt).size() + 1);
}

int32_t seds_pkt_header_string_len(const SedsPacketView * pkt)
{
    if (pkt == nullptr || !seds::valid_type(pkt->ty))
    {
        return SEDS_BAD_ARG;
    }
    return static_cast<int32_t>(seds::packet_header_string(*pkt).size() + 1);
}

SedsResult seds_pkt_header_string(const SedsPacketView * pkt, char * buf, size_t buf_len)
{
    if (pkt == nullptr || buf == nullptr || !seds::valid_type(pkt->ty))
    {
        return SEDS_BAD_ARG;
    }
    return static_cast<SedsResult>(seds::copy_text(seds::packet_header_string(*pkt), buf, buf_len));
}

int32_t seds_error_to_string_len(const int32_t error_code)
{
    return static_cast<int32_t>(seds::error_string(error_code).size() + 1);
}

SedsResult seds_error_to_string(int32_t error_code, char * buf, size_t buf_len)
{
    return static_cast<SedsResult>(seds::copy_text(seds::error_string(error_code), buf, buf_len));
}

int32_t seds_pkt_serialize_len(const SedsPacketView * view)
{
    seds::PacketData packet;
    return seds::packet_from_view(view, packet)
               ? static_cast<int32_t>(seds::serialize_packet(packet).size())
               : SEDS_BAD_ARG;
}

int32_t seds_pkt_serialize(const SedsPacketView * view, uint8_t * out, size_t out_len)
{
    seds::PacketData packet;
    if (!seds::packet_from_view(view, packet))
    {
        return SEDS_BAD_ARG;
    }
    const auto wire = seds::serialize_packet(packet);
    if (out == nullptr || out_len < wire.size())
    {
        return static_cast<int32_t>(wire.size());
    }
    std::memcpy(out, wire.data(), wire.size());
    return static_cast<int32_t>(wire.size());
}

SedsResult seds_pkt_validate_serialized(const uint8_t * bytes, size_t len)
{
    return seds::deserialize_packet(bytes, len).has_value() ? SEDS_OK : SEDS_DESERIALIZE;
}

SedsOwnedPacket * seds_pkt_deserialize_owned(const uint8_t * bytes, size_t len)
{
    auto pkt = seds::deserialize_packet(bytes, len);
    if (!pkt)
    {
        return nullptr;
    }
    auto owned = std::make_unique<SedsOwnedPacket>();
    owned->pkt = std::move(*pkt);
    return owned.release();
}

SedsOwnedHeader * seds_pkt_deserialize_header_owned(const uint8_t * bytes, size_t len)
{
    auto pkt = seds::deserialize_packet(bytes, len);
    if (!pkt)
    {
        return nullptr;
    }
    pkt->payload.clear();
    auto owned = std::make_unique<SedsOwnedHeader>();
    owned->pkt = std::move(*pkt);
    return owned.release();
}

SedsResult seds_owned_pkt_view(const SedsOwnedPacket * pkt, SedsPacketView * out_view)
{
    if (pkt == nullptr || out_view == nullptr)
    {
        return SEDS_BAD_ARG;
    }
    seds::fill_view(pkt->pkt, *out_view);
    return SEDS_OK;
}

SedsResult seds_owned_header_view(const SedsOwnedHeader * h, SedsPacketView * out_view)
{
    if (h == nullptr || out_view == nullptr)
    {
        return SEDS_BAD_ARG;
    }
    seds::fill_view(h->pkt, *out_view);
    return SEDS_OK;
}

void seds_owned_pkt_free(SedsOwnedPacket * pkt) { delete pkt; }
void seds_owned_header_free(SedsOwnedHeader * h) { delete h; }

const void * seds_pkt_bytes_ptr(const SedsPacketView * pkt, size_t * out_len)
{
    if (pkt == nullptr)
    {
        return nullptr;
    }
    if (out_len != nullptr)
    {
        *out_len = pkt->payload_len;
    }
    return pkt->payload;
}

const void * seds_pkt_data_ptr(const SedsPacketView * pkt, size_t elem_size, size_t * out_count)
{
    if (pkt == nullptr || elem_size == 0)
    {
        return nullptr;
    }
    if (out_count != nullptr)
    {
        *out_count = pkt->payload_len / elem_size;
    }
    return pkt->payload;
}

int32_t seds_pkt_copy_bytes(const SedsPacketView * pkt, void * dst, size_t dst_len)
{
    if (pkt == nullptr)
    {
        return SEDS_BAD_ARG;
    }
    if (dst == nullptr || dst_len < pkt->payload_len)
    {
        return static_cast<int32_t>(pkt->payload_len);
    }
    std::memcpy(dst, pkt->payload, pkt->payload_len);
    return static_cast<int32_t>(pkt->payload_len);
}

int32_t seds_pkt_copy_data(const SedsPacketView * pkt, size_t elem_size, void * dst, size_t dst_elems)
{
    if (pkt == nullptr || elem_size == 0)
    {
        return SEDS_BAD_ARG;
    }
    const size_t need = pkt->payload_len / elem_size;
    if (dst == nullptr || dst_elems < need)
    {
        return static_cast<int32_t>(need);
    }
    std::memcpy(dst, pkt->payload, need * elem_size);
    return static_cast<int32_t>(need);
}

int32_t seds_pkt_get_f32(const SedsPacketView * pkt, float * out, size_t out_elems)
{
    return seds::copy_typed_payload(pkt, out, out_elems);
}

int32_t seds_pkt_get_f64(const SedsPacketView * pkt, double * out, size_t out_elems)
{
    return seds::copy_typed_payload(pkt, out, out_elems);
}

int32_t seds_pkt_get_u8(const SedsPacketView * pkt, uint8_t * out, size_t out_elems)
{
    return seds::copy_typed_payload(pkt, out, out_elems);
}

int32_t seds_pkt_get_u16(const SedsPacketView * pkt, uint16_t * out, size_t out_elems)
{
    return seds::copy_typed_payload(pkt, out, out_elems);
}

int32_t seds_pkt_get_u32(const SedsPacketView * pkt, uint32_t * out, size_t out_elems)
{
    return seds::copy_typed_payload(pkt, out, out_elems);
}

int32_t seds_pkt_get_u64(const SedsPacketView * pkt, uint64_t * out, size_t out_elems)
{
    return seds::copy_typed_payload(pkt, out, out_elems);
}

int32_t seds_pkt_get_i8(const SedsPacketView * pkt, int8_t * out, size_t out_elems)
{
    return seds::copy_typed_payload(pkt, out, out_elems);
}

int32_t seds_pkt_get_i16(const SedsPacketView * pkt, int16_t * out, size_t out_elems)
{
    return seds::copy_typed_payload(pkt, out, out_elems);
}

int32_t seds_pkt_get_i32(const SedsPacketView * pkt, int32_t * out, size_t out_elems)
{
    return seds::copy_typed_payload(pkt, out, out_elems);
}

int32_t seds_pkt_get_i64(const SedsPacketView * pkt, int64_t * out, size_t out_elems)
{
    return seds::copy_typed_payload(pkt, out, out_elems);
}

int32_t seds_pkt_get_bool(const SedsPacketView * pkt, bool * out, size_t out_elems)
{
    return seds::copy_typed_payload(pkt, out, out_elems);
}

int32_t seds_pkt_get_string_len(const SedsPacketView * pkt)
{
    return pkt == nullptr ? SEDS_BAD_ARG : static_cast<int32_t>(pkt->payload_len + 1);
}

int32_t seds_pkt_get_string(const SedsPacketView * pkt, char * buf, size_t buf_len)
{
    if (pkt == nullptr)
    {
        return SEDS_BAD_ARG;
    }
    if (buf == nullptr || buf_len <= pkt->payload_len)
    {
        return static_cast<int32_t>(pkt->payload_len + 1);
    }
    std::memcpy(buf, pkt->payload, pkt->payload_len);
    buf[pkt->payload_len] = '\0';
    return static_cast<int32_t>(pkt->payload_len);
}

SedsResult seds_pkt_get_typed(const SedsPacketView * pkt, void * out, size_t count, size_t elem_size, SedsElemKind)
{
    return static_cast<SedsResult>(seds_pkt_copy_data(pkt, elem_size, out, count));
}
} // extern "C"
