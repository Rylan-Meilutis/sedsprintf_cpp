#include "sedsprintf.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct Node Node;
typedef struct Bus Bus;

struct Bus {
    Node *nodes[4];
    size_t count;
    SedsRelay *relay;
    uint32_t relay_side;
    uint8_t pending[32][1024];
    size_t pending_len[32];
    size_t pending_count;
    uint8_t relay_pending[32][1024];
    size_t relay_pending_len[32];
    size_t relay_pending_count;
};

struct Node {
    SedsRouter *router;
    Bus *bus;
    uint32_t side_id;
    unsigned radio_hits;
    unsigned sd_hits;
};

static SedsResult node_tx(const uint8_t *bytes, size_t len, void *user) {
    Node * const node = (Node *)user;
    assert(node != NULL);
    assert(node->bus->pending_count < 32);
    memcpy(node->bus->pending[node->bus->pending_count], bytes, len);
    node->bus->pending_len[node->bus->pending_count] = len;
    node->bus->pending_count++;
    return SEDS_OK;
}

static SedsResult relay_tx(const uint8_t *bytes, size_t len, void *user) {
    Bus * const bus = (Bus *)user;
    assert(bus != NULL);
    assert(bus->relay_pending_count < 32);
    memcpy(bus->relay_pending[bus->relay_pending_count], bytes, len);
    bus->relay_pending_len[bus->relay_pending_count] = len;
    bus->relay_pending_count++;
    return SEDS_OK;
}

static SedsResult radio_handler(const SedsPacketView *pkt, void *user) {
    (void)pkt;
    ((Node *)user)->radio_hits++;
    return SEDS_OK;
}

static SedsResult sd_handler(const SedsPacketView *pkt, void *user) {
    (void)pkt;
    ((Node *)user)->sd_hits++;
    return SEDS_OK;
}

static void pump_bus(Bus *bus) {
    for (size_t i = 0; i < bus->pending_count; ++i) {
        for (size_t n = 0; n < bus->count; ++n) {
            (void)seds_router_rx_serialized_packet_to_queue_from_side(
                bus->nodes[n]->router, bus->nodes[n]->side_id, bus->pending[i], bus->pending_len[i]);
        }
        if (bus->relay) {
            (void)seds_relay_rx_serialized_from_side(bus->relay, bus->relay_side, bus->pending[i], bus->pending_len[i]);
        }
    }
    bus->pending_count = 0;

    for (size_t i = 0; i < bus->relay_pending_count; ++i) {
        for (size_t n = 0; n < bus->count; ++n) {
            (void)seds_router_rx_serialized_packet_to_queue_from_side(
                bus->nodes[n]->router, bus->nodes[n]->side_id, bus->relay_pending[i], bus->relay_pending_len[i]);
        }
    }
    bus->relay_pending_count = 0;
}

int main(void) {
    Bus bus1 = {0};
    Bus bus2 = {0};
    bus1.relay = seds_relay_new(NULL, NULL);
    assert(bus1.relay != NULL);
    bus2.relay = bus1.relay;
    bus1.relay_side = (uint32_t)seds_relay_add_side_serialized(bus1.relay, "bus1", 4, relay_tx, &bus1, false);
    bus2.relay_side = (uint32_t)seds_relay_add_side_serialized(bus1.relay, "bus2", 4, relay_tx, &bus2, false);

    Node a = {0}, b = {0}, c = {0}, d = {0};
    a.bus = &bus1; b.bus = &bus1; c.bus = &bus1; d.bus = &bus2;

    const SedsLocalEndpointDesc a_handlers[] = {{SEDS_EP_RADIO, radio_handler, NULL, &a}};
    const SedsLocalEndpointDesc b_handlers[] = {{SEDS_EP_SD_CARD, sd_handler, NULL, &b}};
    const SedsLocalEndpointDesc d_handlers[] = {{SEDS_EP_RADIO, radio_handler, NULL, &d}};

    a.router = seds_router_new(Seds_RM_Sink, NULL, NULL, a_handlers, 1);
    b.router = seds_router_new(Seds_RM_Sink, NULL, NULL, b_handlers, 1);
    c.router = seds_router_new(Seds_RM_Sink, NULL, NULL, NULL, 0);
    d.router = seds_router_new(Seds_RM_Sink, NULL, NULL, d_handlers, 1);
    assert(a.router && b.router && c.router && d.router);

    a.side_id = (uint32_t)seds_router_add_side_serialized(a.router, "BUS", 3, node_tx, &a, false);
    b.side_id = (uint32_t)seds_router_add_side_serialized(b.router, "BUS", 3, node_tx, &b, false);
    c.side_id = (uint32_t)seds_router_add_side_serialized(c.router, "BUS", 3, node_tx, &c, false);
    d.side_id = (uint32_t)seds_router_add_side_serialized(d.router, "BUS", 3, node_tx, &d, false);

    bus1.nodes[bus1.count++] = &a;
    bus1.nodes[bus1.count++] = &b;
    bus1.nodes[bus1.count++] = &c;
    bus2.nodes[bus2.count++] = &d;

    {
        const float gps[3] = {1.0f, 2.0f, 3.0f};
        const float imu[6] = {1, 2, 3, 4, 5, 6};
        const float batt[2] = {7.0f, 8.0f};
        assert(seds_router_log_f32(a.router, SEDS_DT_GPS_DATA, gps, 3) == SEDS_OK);
        assert(seds_router_log_f32(b.router, SEDS_DT_IMU_DATA, imu, 6) == SEDS_OK);
        assert(seds_router_log_f32(c.router, SEDS_DT_BATTERY_STATUS, batt, 2) == SEDS_OK);
    }

    for (int i = 0; i < 6; ++i) {
        pump_bus(&bus1);
        pump_bus(&bus2);
        assert(seds_router_process_all_queues(a.router) == SEDS_OK);
        assert(seds_router_process_all_queues(b.router) == SEDS_OK);
        assert(seds_router_process_all_queues(c.router) == SEDS_OK);
        assert(seds_router_process_all_queues(d.router) == SEDS_OK);
        assert(seds_relay_process_all_queues(bus1.relay) == SEDS_OK);
    }

    assert(a.radio_hits >= 2);
    assert(b.sd_hits >= 2);
    assert(d.radio_hits >= 1);

    seds_router_free(a.router);
    seds_router_free(b.router);
    seds_router_free(c.router);
    seds_router_free(d.router);
    seds_relay_free(bus1.relay);

    printf("c system ok\n");
    return 0;
}
