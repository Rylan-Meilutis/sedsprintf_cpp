#include "src/c_api.hpp"
#include "src/discovery_helpers.hpp"
#include "src/config.hpp"
#include "src/macros.hpp"
#include "src/packet.hpp"
#include "src/queue.hpp"
#include "src/relay.hpp"
#include "src/router.hpp"
#include "src/small_payload.hpp"
#include "src/timesync.hpp"

#include <gtest/gtest.h>

namespace {

TEST(CppWrapperTest, SmallPayloadUsesInlineThenHeap) {
    const std::array<uint8_t, 4> small{1, 2, 3, 4};
    seds::SmallPayload payload(small);
    ASSERT_FALSE(payload.using_heap());
    ASSERT_EQ(payload.size(), small.size());

    std::vector<uint8_t> large(128, 0x5A);
    payload.assign(large);
    ASSERT_TRUE(payload.using_heap());
    ASSERT_EQ(payload.size(), large.size());
}

TEST(CppWrapperTest, PacketRoundTripAndFormatting) {
    const std::array<float, 3> values{1.0f, 2.5f, 3.25f};
    const auto pkt = seds::Packet::from_f32_slice(SEDS_DT_GPS_DATA, values,
                                                  {SEDS_EP_SD_CARD, SEDS_EP_RADIO}, 42, "CPP");
    const auto wire = pkt.serialize();
    const auto decoded = seds::Packet::deserialize(wire);
    ASSERT_TRUE(decoded.has_value());
    ASSERT_EQ(decoded->type(), SEDS_DT_GPS_DATA);
    ASSERT_EQ(decoded->endpoints().size(), 2u);
    ASSERT_NE(decoded->to_string().find("2.50000000"), std::string::npos);
    ASSERT_NE(decoded->header_string().find("GPS_DATA"), std::string::npos);
}

TEST(CppWrapperTest, ConfigLookupExposesGeneratedMetadata) {
    const auto* info = seds::find_type_info(SEDS_DT_GPS_DATA);
    ASSERT_NE(info, nullptr);
    ASSERT_STREQ(info->name, "GPS_DATA");
    const auto endpoint = seds::endpoint_by_name("RADIO");
    ASSERT_TRUE(endpoint.has_value());
    ASSERT_EQ(*endpoint, SEDS_EP_RADIO);
}

TEST(CppWrapperTest, RouterUsesRaiiCallbacks) {
    seds::Router router(Seds_RM_Sink);
    std::vector<seds::Packet> seen;
    ASSERT_GE(router.add_side_packet("pkt", [&](const seds::Packet& pkt) {
                  seen.push_back(pkt);
                  return SEDS_OK;
              }),
              0);

    const std::array<float, 3> values{1.0f, 2.0f, 3.0f};
    const auto pkt = seds::Packet::from_f32_slice(SEDS_DT_GPS_DATA, values,
                                                  {SEDS_EP_SD_CARD, SEDS_EP_RADIO}, 5);
    ASSERT_EQ(router.log(pkt, true), SEDS_OK);
    ASSERT_EQ(router.process_all(), SEDS_OK);
    ASSERT_EQ(seen.size(), 1u);
}

TEST(CppWrapperTest, RelayUsesRaiiCallbacks) {
    seds::Relay relay;
    std::vector<seds::Packet> seen;
    ASSERT_GE(relay.add_side_packet("dst", [&](const seds::Packet& pkt) {
                  seen.push_back(pkt);
                  return SEDS_OK;
              }),
              0);
    ASSERT_GE(relay.add_side_packet("src", [](const seds::Packet&) { return SEDS_OK; }), 0);

    const std::array<float, 3> values{4.0f, 5.0f, 6.0f};
    const auto pkt = seds::Packet::from_f32_slice(SEDS_DT_GPS_DATA, values,
                                                  {SEDS_EP_SD_CARD}, 6);
    ASSERT_EQ(relay.receive_from_side(1, pkt), SEDS_OK);
    ASSERT_EQ(relay.process_all(), SEDS_OK);
    ASSERT_EQ(seen.size(), 1u);
}

TEST(CppWrapperTest, RouterExportsTopologyAndClearsNetworkState) {
    seds::Router router(Seds_RM_Sink);
    const auto side_a = router.add_side_packet("A", [](const seds::Packet&) { return SEDS_OK; });
    ASSERT_GE(side_a, 0);

    const std::array<uint32_t, 1> discovery_payload{SEDS_EP_SD_CARD};
    const auto discovery = seds::Packet(SEDS_DT_DISCOVERY_ANNOUNCE, {SEDS_EP_DISCOVERY}, "REMOTE_A", 0,
                                        std::span<const uint8_t>(
                                            reinterpret_cast<const uint8_t*>(discovery_payload.data()),
                                            sizeof(discovery_payload)));
    const auto discovery_view = discovery.view();
    ASSERT_EQ(seds_router_receive_from_side(router.raw(), static_cast<uint32_t>(side_a), &discovery_view), SEDS_OK);
    const auto topology = router.export_topology();
    ASSERT_FALSE(topology.routes.empty());

    ASSERT_EQ(seds_router_set_local_network_datetime_millis(router.raw(), 2026, 3, 21, 12, 34, 56, 250), SEDS_OK);
    ASSERT_EQ(router.clear_local_network_time(), SEDS_OK);
    uint64_t now_ms = 0;
    ASSERT_EQ(seds_router_get_network_time_ms(router.raw(), &now_ms), SEDS_ERR);
}

TEST(CppWrapperTest, RelayExportsTopologyAndSupportsSideOptions) {
    seds::Relay relay;
    const auto side_a = relay.add_side_packet("A", [](const seds::Packet&) { return SEDS_OK; });
    ASSERT_GE(side_a, 0);
    ASSERT_GE(relay.add_side_packet("LL", [](const seds::Packet&) { return SEDS_OK; },
                                    seds::Relay::SideOptions{false, true}),
              0);

    const std::array<uint32_t, 1> discovery_payload{SEDS_EP_RADIO};
    const auto discovery = seds::Packet(SEDS_DT_DISCOVERY_ANNOUNCE, {SEDS_EP_DISCOVERY}, "REMOTE_A", 0,
                                        std::span<const uint8_t>(
                                            reinterpret_cast<const uint8_t*>(discovery_payload.data()),
                                            sizeof(discovery_payload)));
    ASSERT_EQ(relay.receive_from_side(static_cast<uint32_t>(side_a), discovery), SEDS_OK);
    ASSERT_EQ(relay.process_all(), SEDS_OK);
    const auto topology = relay.export_topology();
    ASSERT_FALSE(topology.routes.empty());
}

TEST(CppWrapperTest, DiscoveryHelpersRoundTrip) {
    const std::array<uint32_t, 3> endpoints{SEDS_EP_RADIO, SEDS_EP_SD_CARD, SEDS_EP_RADIO};
    const auto announce = seds::build_discovery_announce("DISC", 1, endpoints);
    const auto decoded_endpoints = seds::decode_discovery_announce(announce);
    ASSERT_EQ(decoded_endpoints.size(), 2u);
    ASSERT_EQ(decoded_endpoints[0], SEDS_EP_SD_CARD);
    ASSERT_EQ(decoded_endpoints[1], SEDS_EP_RADIO);

    const std::array<std::string_view, 3> sources{"SRC_A", "SRC_B", "SRC_A"};
    const auto src_pkt = seds::build_discovery_timesync_sources("DISC", 2, sources);
    const auto decoded_sources = seds::decode_discovery_timesync_sources(src_pkt);
    ASSERT_EQ(decoded_sources.size(), 2u);
    ASSERT_EQ(decoded_sources[0], "SRC_A");
    ASSERT_EQ(decoded_sources[1], "SRC_B");
}

TEST(CppWrapperTest, TimeSyncHelpersRoundTripAndMath) {
    const auto announce = seds::build_timesync_announce("TS", 1, 7, 1700000000000ull);
    const auto announce_fields = seds::decode_timesync_announce(announce);
    ASSERT_EQ(announce_fields.priority, 7u);
    ASSERT_EQ(announce_fields.time_ms, 1700000000000ull);

    const auto request = seds::build_timesync_request("TS", 2, 11, 22);
    const auto request_fields = seds::decode_timesync_request(request);
    ASSERT_EQ(request_fields.seq, 11u);
    ASSERT_EQ(request_fields.t1_ms, 22u);

    const auto response = seds::build_timesync_response("TS", 3, 11, 22, 33, 44);
    const auto response_fields = seds::decode_timesync_response(response);
    ASSERT_EQ(response_fields.seq, 11u);
    ASSERT_EQ(response_fields.t2_ms, 33u);
    ASSERT_EQ(response_fields.t3_ms, 44u);

    const auto sample = seds::compute_offset_delay(10, 20, 30, 40);
    ASSERT_EQ(sample.offset_ms, 0);
    ASSERT_EQ(sample.delay_ms, 20u);

    const auto [estimated_network_ms, one_way_delay] = seds::estimate_network_time(10, 40, 20, 30);
    ASSERT_EQ(estimated_network_ms, 40u);
    ASSERT_EQ(one_way_delay, 10u);
}

TEST(CppWrapperTest, MirrorHeadersExposeExpectedGlue) {
    const auto mode_value = seds::to_underlying(Seds_RM_Sink);
    ASSERT_EQ(mode_value, 0);

    const auto parsed = seds::enum_from_underlying<SedsRouterMode>(0, 0, 2);
    ASSERT_TRUE(parsed.has_value());
    ASSERT_EQ(*parsed, Seds_RM_Sink);

    const seds::TxQueue tx_queue;
    const seds::RxQueue rx_queue;
    ASSERT_TRUE(tx_queue.empty());
    ASSERT_TRUE(rx_queue.empty());

    ASSERT_EQ(SEDS_OK, 0);
}

}  // namespace
