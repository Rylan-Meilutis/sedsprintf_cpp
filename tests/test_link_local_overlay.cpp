#include "sedsprintf.h"
#include "src/internal.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

// The overlay tests are compiled against an alternate generated schema target.
// Provide JetBrains-indexer-only fallbacks so the default index doesn't report
// unresolved symbols, without affecting real builds.
#if defined(__JETBRAINS_IDE__)
#  ifndef SEDS_EP_BOARD_LOCAL
#    define SEDS_EP_BOARD_LOCAL (0xFFFF0001u)
#  endif

#  ifndef SEDS_DT_BOARD_LOCAL_FRAME
#    define SEDS_DT_BOARD_LOCAL_FRAME (0xFFFF0001u)
#  endif
#endif

namespace {

struct RouterDeleter {
    void operator()(SedsRouter* router) const {
        if (router != nullptr) {
            seds_router_free(router);
        }
    }
};

struct RelayDeleter {
    void operator()(SedsRelay* relay) const {
        if (relay != nullptr) {
            seds_relay_free(relay);
        }
    }
};

using RouterHandle = std::unique_ptr<SedsRouter, RouterDeleter>;
using RelayHandle = std::unique_ptr<SedsRelay, RelayDeleter>;

struct PacketCopy {
    uint32_t ty{};
    std::string sender;
    std::vector<uint32_t> endpoints;
    uint64_t timestamp{};
    std::vector<uint8_t> payload;
};

struct PacketCapture {
    std::vector<PacketCopy> packets;
};

SedsResult capture_serialized(const uint8_t* bytes, size_t len, void* user) {
    auto* capture = static_cast<PacketCapture*>(user);
    const auto pkt = seds::deserialize_packet(bytes, len);
    if (!pkt.has_value()) {
        return SEDS_DESERIALIZE;
    }
    capture->packets.push_back(PacketCopy{
        pkt->ty,
        pkt->sender,
        pkt->endpoints,
        pkt->timestamp,
        pkt->payload,
    });
    return SEDS_OK;
}

SedsResult capture_packet(const SedsPacketView* pkt, void* user) {
    auto* capture = static_cast<PacketCapture*>(user);
    capture->packets.push_back(PacketCopy{
        pkt->ty,
        std::string(pkt->sender ? pkt->sender : "", pkt->sender_len),
        std::vector<uint32_t>(pkt->endpoints, pkt->endpoints + pkt->num_endpoints),
        pkt->timestamp,
        std::vector<uint8_t>(pkt->payload, pkt->payload + pkt->payload_len),
    });
    return SEDS_OK;
}

SedsResult noop_handler(const SedsPacketView*, void*) {
    return SEDS_OK;
}

std::vector<uint32_t> decode_discovery_payload(const PacketCopy& pkt) {
    std::vector<uint32_t> endpoints;
    for (size_t off = 0; off + sizeof(uint32_t) <= pkt.payload.size(); off += sizeof(uint32_t)) {
        uint32_t endpoint = 0;
        std::memcpy(&endpoint, pkt.payload.data() + off, sizeof(endpoint));
        endpoints.push_back(endpoint);
    }
    return endpoints;
}

PacketCopy make_discovery_packet(const char* sender, uint32_t advertised_endpoint) {
    PacketCopy pkt{};
    pkt.ty = SEDS_DT_DISCOVERY_ANNOUNCE;
    pkt.sender = sender;
    pkt.endpoints = {SEDS_EP_DISCOVERY};
    pkt.payload.resize(sizeof(uint32_t));
    std::memcpy(pkt.payload.data(), &advertised_endpoint, sizeof(advertised_endpoint));
    return pkt;
}

SedsPacketView to_view(const PacketCopy& pkt) {
    return SedsPacketView{
        .ty = pkt.ty,
        .data_size = pkt.payload.size(),
        .sender = pkt.sender.c_str(),
        .sender_len = pkt.sender.size(),
        .endpoints = pkt.endpoints.data(),
        .num_endpoints = pkt.endpoints.size(),
        .timestamp = pkt.timestamp,
        .payload = pkt.payload.data(),
        .payload_len = pkt.payload.size(),
    };
}

TEST(LinkLocalOverlayTest, LinkLocalOnlyPacketsStayOnLinkLocalSides) {
    PacketCapture net{};
    PacketCapture link_local{};
    RouterHandle router(seds_router_new(Seds_RM_Sink, nullptr, nullptr, nullptr, 0));
    ASSERT_NE(router, nullptr);

    const int32_t side_net = seds_router_add_side_serialized(router.get(), "NET", 3, capture_serialized, &net, false);
    const int32_t side_ll = seds_router_add_side_serialized(router.get(), "LL", 2, capture_serialized, &link_local, false);
    ASSERT_GE(side_net, 0);
    ASSERT_GE(side_ll, 0);
    ASSERT_EQ(seds_router_set_side_link_local_enabled(router.get(), side_ll, true), SEDS_OK);
    ASSERT_TRUE(router->sides[side_ll].link_local_enabled);

    const std::array<uint8_t, 5> payload{'h', 'e', 'l', 'l', 'o'};
    const uint32_t endpoint = SEDS_EP_BOARD_LOCAL;
    const SedsPacketView pkt{
        .ty = SEDS_DT_BOARD_LOCAL_FRAME,
        .data_size = payload.size(),
        .sender = "IPC_NODE",
        .sender_len = 8,
        .endpoints = &endpoint,
        .num_endpoints = 1,
        .timestamp = 7,
        .payload = payload.data(),
        .payload_len = payload.size(),
    };

    ASSERT_EQ(seds_router_transmit_message(router.get(), &pkt), SEDS_OK);
    ASSERT_EQ(seds_router_process_tx_queue(router.get()), SEDS_OK);
    ASSERT_TRUE(net.packets.empty());
    ASSERT_EQ(link_local.packets.size(), 1u);
}

TEST(LinkLocalOverlayTest, LinkLocalRoutesIgnoreNonLinkLocalDiscoveryCandidates) {
    PacketCapture net{};
    PacketCapture link_local{};
    RouterHandle router(seds_router_new(Seds_RM_Sink, nullptr, nullptr, nullptr, 0));
    ASSERT_NE(router, nullptr);

    const int32_t side_net = seds_router_add_side_serialized(router.get(), "NET", 3, capture_serialized, &net, false);
    const int32_t side_ll = seds_router_add_side_serialized(router.get(), "LL", 2, capture_serialized, &link_local, false);
    ASSERT_GE(side_net, 0);
    ASSERT_GE(side_ll, 0);
    ASSERT_EQ(seds_router_set_side_link_local_enabled(router.get(), side_ll, true), SEDS_OK);
    ASSERT_TRUE(router->sides[side_ll].link_local_enabled);

    const PacketCopy from_net = make_discovery_packet("NET_NODE", SEDS_EP_BOARD_LOCAL);
    const PacketCopy from_ll = make_discovery_packet("LL_NODE", SEDS_EP_BOARD_LOCAL);
    auto view_net = to_view(from_net);
    auto view_ll = to_view(from_ll);
    ASSERT_EQ(seds_router_receive_from_side(router.get(), static_cast<uint32_t>(side_net), &view_net), SEDS_OK);
    ASSERT_EQ(seds_router_receive_from_side(router.get(), static_cast<uint32_t>(side_ll), &view_ll), SEDS_OK);

    const std::array<uint8_t, 10> payload{'s', 't', 'a', 'y', '-', 'l', 'o', 'c', 'a', 'l'};
    const uint32_t endpoint = SEDS_EP_BOARD_LOCAL;
    const SedsPacketView pkt{
        .ty = SEDS_DT_BOARD_LOCAL_FRAME,
        .data_size = payload.size(),
        .sender = "IPC_NODE",
        .sender_len = 8,
        .endpoints = &endpoint,
        .num_endpoints = 1,
        .timestamp = 8,
        .payload = payload.data(),
        .payload_len = payload.size(),
    };

    ASSERT_EQ(seds_router_transmit_message(router.get(), &pkt), SEDS_OK);
    ASSERT_EQ(seds_router_process_tx_queue(router.get()), SEDS_OK);
    ASSERT_TRUE(net.packets.empty());
    ASSERT_EQ(link_local.packets.size(), 1u);
}

TEST(LinkLocalOverlayTest, RelayLinkLocalRoutesIgnoreNonLinkLocalCandidates) {
    PacketCapture net{};
    PacketCapture link_local{};
    RelayHandle relay(seds_relay_new(nullptr, nullptr));
    ASSERT_NE(relay, nullptr);

    const int32_t side_net = seds_relay_add_side_packet(relay.get(), "NET", 3, capture_packet, &net, false);
    const int32_t side_ll = seds_relay_add_side_packet(relay.get(), "LL", 2, capture_packet, &link_local, false);
    const int32_t side_src = seds_relay_add_side_packet(relay.get(), "SRC", 3, noop_handler, nullptr, false);
    ASSERT_GE(side_net, 0);
    ASSERT_GE(side_ll, 0);
    ASSERT_GE(side_src, 0);
    ASSERT_EQ(seds_relay_set_side_link_local_enabled(relay.get(), side_ll, true), SEDS_OK);

    const PacketCopy from_net = make_discovery_packet("NET_NODE", SEDS_EP_BOARD_LOCAL);
    const PacketCopy from_ll = make_discovery_packet("LL_NODE", SEDS_EP_BOARD_LOCAL);
    auto view_net = to_view(from_net);
    auto view_ll = to_view(from_ll);
    ASSERT_EQ(seds_relay_rx_packet_from_side(relay.get(), static_cast<uint32_t>(side_net), &view_net), SEDS_OK);
    ASSERT_EQ(seds_relay_rx_packet_from_side(relay.get(), static_cast<uint32_t>(side_ll), &view_ll), SEDS_OK);
    ASSERT_EQ(seds_relay_process_all_queues(relay.get()), SEDS_OK);
    net.packets.clear();
    link_local.packets.clear();

    const std::array<uint8_t, 11> payload{'r', 'e', 'l', 'a', 'y', '-', 'l', 'o', 'c', 'a', 'l'};
    const uint32_t endpoint = SEDS_EP_BOARD_LOCAL;
    const SedsPacketView pkt{
        .ty = SEDS_DT_BOARD_LOCAL_FRAME,
        .data_size = payload.size(),
        .sender = "IPC_NODE",
        .sender_len = 8,
        .endpoints = &endpoint,
        .num_endpoints = 1,
        .timestamp = 9,
        .payload = payload.data(),
        .payload_len = payload.size(),
    };

    ASSERT_EQ(seds_relay_rx_packet_from_side(relay.get(), static_cast<uint32_t>(side_src), &pkt), SEDS_OK);
    ASSERT_EQ(seds_relay_process_all_queues(relay.get()), SEDS_OK);
    ASSERT_TRUE(net.packets.empty());
    ASSERT_EQ(link_local.packets.size(), 1u);
}

TEST(LinkLocalOverlayTest, DiscoveryHidesLinkLocalEndpointsFromNetworkSides) {
    PacketCapture net{};
    PacketCapture link_local{};
    const SedsLocalEndpointDesc handlers[] = {
        {.endpoint = SEDS_EP_BOARD_LOCAL, .packet_handler = noop_handler, .serialized_handler = nullptr, .user = nullptr},
        {.endpoint = SEDS_EP_RADIO, .packet_handler = noop_handler, .serialized_handler = nullptr, .user = nullptr},
    };
    RouterHandle router(seds_router_new(Seds_RM_Sink, nullptr, nullptr, handlers, 2));
    ASSERT_NE(router, nullptr);

    const int32_t side_net = seds_router_add_side_packet(router.get(), "NET", 3, capture_packet, &net, false);
    const int32_t side_ll = seds_router_add_side_packet(router.get(), "LL", 2, capture_packet, &link_local, false);
    ASSERT_GE(side_net, 0);
    ASSERT_GE(side_ll, 0);
    ASSERT_EQ(seds_router_set_side_link_local_enabled(router.get(), side_ll, true), SEDS_OK);

    ASSERT_EQ(seds_router_announce_discovery(router.get()), SEDS_OK);
    ASSERT_EQ(seds_router_process_tx_queue(router.get()), SEDS_OK);
    ASSERT_EQ(net.packets.size(), 2u);
    ASSERT_EQ(link_local.packets.size(), 2u);

    const auto net_it = std::find_if(net.packets.begin(), net.packets.end(), [](const auto& packet) {
        return packet.ty == SEDS_DT_DISCOVERY_ANNOUNCE;
    });
    const auto ll_it = std::find_if(link_local.packets.begin(), link_local.packets.end(), [](const auto& packet) {
        return packet.ty == SEDS_DT_DISCOVERY_ANNOUNCE;
    });
    ASSERT_NE(net_it, net.packets.end());
    ASSERT_NE(ll_it, link_local.packets.end());

    const auto net_eps = decode_discovery_payload(*net_it);
    const auto ll_eps = decode_discovery_payload(*ll_it);
    ASSERT_TRUE(std::find(net_eps.begin(), net_eps.end(), SEDS_EP_RADIO) != net_eps.end());
    ASSERT_TRUE(std::find(net_eps.begin(), net_eps.end(), SEDS_EP_BOARD_LOCAL) == net_eps.end());
    ASSERT_TRUE(std::find(ll_eps.begin(), ll_eps.end(), SEDS_EP_BOARD_LOCAL) != ll_eps.end());
}

}  // namespace
