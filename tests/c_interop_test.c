#include "sedsprintf.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    unsigned tx_hits;
    unsigned local_hits;
    uint8_t last_frame[1024];
    size_t last_len;
} Capture;

static SedsResult tx_capture(const uint8_t *bytes, size_t len, void *user) {
    Capture *cap = (Capture *)user;
    assert(cap != NULL);
    assert(len < sizeof(cap->last_frame));
    memcpy(cap->last_frame, bytes, len);
    cap->last_len = len;
    cap->tx_hits++;
    return SEDS_OK;
}

static SedsResult local_capture(const SedsPacketView *pkt, void *user) {
    Capture *cap = (Capture *)user;
    assert(cap != NULL);
    assert(pkt != NULL);
    cap->local_hits++;
    return SEDS_OK;
}

int main(void) {
    Capture cap = {0};
    const SedsLocalEndpointDesc handlers[] = {
        {.endpoint = SEDS_EP_SD_CARD, .packet_handler = local_capture, .serialized_handler = NULL, .user = &cap},
    };

    SedsRouter *r = seds_router_new(Seds_RM_Sink, NULL, NULL, handlers, 1);
    assert(r != NULL);
    assert(seds_router_add_side_serialized(r, "BUS", 3, tx_capture, &cap, true) >= 0);

    const float gps[3] = {1.0f, 2.0f, 3.0f};
    assert(seds_router_log_f32(r, SEDS_DT_GPS_DATA, gps, 3) == SEDS_OK);
    assert(cap.local_hits == 1);
    assert(cap.tx_hits == 1);
    assert(seds_pkt_validate_serialized(cap.last_frame, cap.last_len) == SEDS_OK);
    assert(seds_router_set_side_egress_enabled(r, 0, false) == SEDS_OK);
    assert(seds_router_log_f32(r, SEDS_DT_GPS_DATA, gps, 3) == SEDS_OK);
    assert(cap.tx_hits == 1);
    assert(seds_router_set_side_egress_enabled(r, 0, true) == SEDS_OK);

    SedsOwnedPacket *owned = seds_pkt_deserialize_owned(cap.last_frame, cap.last_len);
    assert(owned != NULL);
    SedsPacketView view;
    assert(seds_owned_pkt_view(owned, &view) == SEDS_OK);
    assert(view.ty == SEDS_DT_GPS_DATA);
    assert(view.num_endpoints == 2);
    seds_owned_pkt_free(owned);

    SedsOwnedHeader *hdr = seds_pkt_deserialize_header_owned(cap.last_frame, cap.last_len);
    assert(hdr != NULL);
    assert(seds_owned_header_view(hdr, &view) == SEDS_OK);
    assert(view.payload_len == 0);
    seds_owned_header_free(hdr);

    const int32_t needed = seds_pkt_to_string_len(&view);
    assert(needed > 0);
    char buf[256];
    assert((size_t)needed <= sizeof(buf));
    assert(seds_pkt_to_string(&view, buf, sizeof(buf)) == SEDS_OK);
    assert(strlen(buf) > 0);

    seds_router_free(r);
    printf("c interop ok\n");
    return 0;
}
