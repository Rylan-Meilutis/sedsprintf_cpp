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
    SedsResult set_sender(std::string_view sender);
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
