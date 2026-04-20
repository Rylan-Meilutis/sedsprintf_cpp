#pragma once

#include "packet.hpp"
#include "discovery.hpp"
#include "sedsprintf.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace seds {

class Router {
public:
    struct SideOptions {
        bool reliable_enabled{false};
        bool link_local_enabled{false};
    };
    struct TimeSyncOptions {
        uint32_t role{0};
        uint64_t priority{100};
        uint64_t source_timeout_ms{5000};
        uint64_t announce_interval_ms{1000};
        uint64_t request_interval_ms{1000};
    };
    using PacketHandler = std::function<SedsResult(const Packet&)>;
    using SerializedHandler = std::function<SedsResult(std::span<const uint8_t>)>;

    explicit Router(SedsRouterMode mode = Seds_RM_Sink);
    ~Router() = default;

    Router(const Router&) = delete;
    Router& operator=(const Router&) = delete;
    Router(Router&&) noexcept = default;
    Router& operator=(Router&&) noexcept = default;

    int32_t add_side_serialized(std::string name, SerializedHandler handler, bool reliable = false);
    int32_t add_side_packet(std::string name, PacketHandler handler, bool reliable = false);
    int32_t add_side_serialized(const std::string& name, SerializedHandler handler, SideOptions options);
    int32_t add_side_packet(const std::string& name, PacketHandler handler, SideOptions options);

    SedsResult log(const Packet& packet, bool queue = false);
    SedsResult receive(const Packet& packet);
    SedsResult process_all();
    SedsResult periodic(uint32_t timeout_ms);
    SedsResult periodic_no_timesync(uint32_t timeout_ms);
    SedsResult set_sender(std::string_view sender);
    SedsResult get_network_time_ms(uint64_t& out_ms) const;
    SedsResult get_network_time(SedsNetworkTime& out) const;
    SedsResult configure_timesync(bool enabled);
    SedsResult configure_timesync(bool enabled, const TimeSyncOptions& options);
    SedsResult poll_timesync(bool* out_did_queue = nullptr);
    SedsResult announce_discovery();
    SedsResult poll_discovery(bool* out_did_queue = nullptr);
    SedsResult set_local_network_time(const SedsNetworkTime& time);
    SedsResult set_local_network_date(int32_t year, uint8_t month, uint8_t day);
    SedsResult set_local_network_time_hm(uint8_t hour, uint8_t minute);
    SedsResult set_local_network_time_hms(uint8_t hour, uint8_t minute, uint8_t second);
    SedsResult set_local_network_time_hms_millis(uint8_t hour, uint8_t minute, uint8_t second, uint16_t millisecond);
    SedsResult set_local_network_time_hms_nanos(uint8_t hour, uint8_t minute, uint8_t second, uint32_t nanosecond);
    SedsResult set_local_network_datetime(int32_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute,
                                          uint8_t second);
    SedsResult set_local_network_datetime_millis(int32_t year, uint8_t month, uint8_t day, uint8_t hour,
                                                 uint8_t minute, uint8_t second, uint16_t millisecond);
    SedsResult set_local_network_datetime_nanos(int32_t year, uint8_t month, uint8_t day, uint8_t hour,
                                                uint8_t minute, uint8_t second, uint32_t nanosecond);
    SedsResult clear_local_network_time();
    SedsResult clear_network_time_source(std::string_view source);
    [[nodiscard]] TopologySnapshot export_topology() const;

    [[nodiscard]] SedsRouter* raw() const { return router_.get(); }

private:
    struct State;
    static SedsResult packet_bridge(const SedsPacketView* pkt, void* user);
    static SedsResult serialized_bridge(const uint8_t* bytes, size_t len, void* user);

    std::shared_ptr<State> state_;
    struct Deleter {
        void operator()(SedsRouter* router) const {
            if (router != nullptr) {
                seds_router_free(router);
            }
        }
    };
    std::unique_ptr<SedsRouter, Deleter> router_;
};

}  // namespace seds
