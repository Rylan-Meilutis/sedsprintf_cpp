#pragma once

#include "packet.hpp"
#include "discovery.hpp"
#include "sedsprintf.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace seds {

class Relay {
public:
    struct SideOptions {
        bool reliable_enabled{false};
        bool link_local_enabled{false};
    };
    using PacketHandler = std::function<SedsResult(const Packet&)>;
    using SerializedHandler = std::function<SedsResult(std::span<const uint8_t>)>;

    Relay();
    ~Relay() = default;

    Relay(const Relay&) = delete;
    Relay& operator=(const Relay&) = delete;
    Relay(Relay&&) noexcept = default;
    Relay& operator=(Relay&&) noexcept = default;

    int32_t add_side_serialized(std::string name, SerializedHandler handler, bool reliable = false);
    int32_t add_side_packet(std::string name, PacketHandler handler, bool reliable = false);
    int32_t add_side_serialized(const std::string& name, SerializedHandler handler, SideOptions options);
    int32_t add_side_packet(const std::string& name, PacketHandler handler, SideOptions options);

    SedsResult receive_from_side(uint32_t side_id, const Packet& packet);
    SedsResult process_all();
    SedsResult announce_discovery();
    SedsResult poll_discovery(bool* out_did_queue = nullptr);
    SedsResult periodic(uint32_t timeout_ms);
    [[nodiscard]] TopologySnapshot export_topology() const;

    [[nodiscard]] SedsRelay* raw() const { return relay_.get(); }

private:
    struct State;
    static SedsResult packet_bridge(const SedsPacketView* pkt, void* user);
    static SedsResult serialized_bridge(const uint8_t* bytes, size_t len, void* user);

    std::shared_ptr<State> state_;
    struct Deleter {
        void operator()(SedsRelay* relay) const {
            if (relay != nullptr) {
                seds_relay_free(relay);
            }
        }
    };
    std::unique_ptr<SedsRelay, Deleter> relay_;
};

}  // namespace seds
