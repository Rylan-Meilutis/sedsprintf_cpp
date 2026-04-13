#include "internal.hpp"

#include <cstring>

namespace seds {

void handle_timesync_packet(SedsRouter& r, const PacketData& pkt, std::optional<int32_t> src_side) {
    const uint64_t now = r.now_ms();
    if (pkt.ty == SEDS_DT_TIME_SYNC_ANNOUNCE && pkt.payload.size() >= 16) {
        uint64_t priority = 0;
        uint64_t unix_ms = 0;
        std::memcpy(&priority, pkt.payload.data(), 8);
        std::memcpy(&unix_ms, pkt.payload.data() + 8, 8);
        r.timesync.sources[pkt.sender] = {priority, now, unix_ms};
        if (!r.timesync.has_network_time || priority < r.timesync.current_source_priority ||
            r.timesync.current_source.empty() ||
            now - r.timesync.sources[r.timesync.current_source].last_seen_ms > r.timesync.source_timeout_ms) {
            r.timesync.has_network_time = true;
            r.timesync.network_anchor_local_ms = now;
            r.timesync.network_anchor_unix_ms = unix_ms;
            r.timesync.current_source = pkt.sender;
            r.timesync.current_source_priority = priority;
        }
    } else if (pkt.ty == SEDS_DT_TIME_SYNC_REQUEST && pkt.payload.size() >= 16) {
        if (r.timesync.enabled && (r.timesync.role == 1u || r.timesync.role == 2u) && r.timesync.has_network_time) {
            uint64_t seq = 0;
            uint64_t t1 = 0;
            std::memcpy(&seq, pkt.payload.data(), 8);
            std::memcpy(&t1, pkt.payload.data() + 8, 8);
            std::vector<uint8_t> resp;
            append_le<uint64_t>(seq, resp);
            append_le<uint64_t>(t1, resp);
            append_le<uint64_t>(r.current_network_ms(), resp);
            append_le<uint64_t>(r.current_network_ms(), resp);
            auto pkt = make_internal_packet(SEDS_DT_TIME_SYNC_RESPONSE, now, std::move(resp));
            pkt.sender = r.sender;
            enqueue_tx(r.tx_queue, r.tx_queue_bytes,
                       {std::move(pkt), std::nullopt, src_side, false});
        }
    } else if (pkt.ty == SEDS_DT_TIME_SYNC_RESPONSE && pkt.payload.size() >= 32) {
        uint64_t t3 = 0;
        std::memcpy(&t3, pkt.payload.data() + 24, 8);
        r.timesync.has_network_time = true;
        r.timesync.network_anchor_local_ms = now;
        r.timesync.network_anchor_unix_ms = t3;
    }
}

}  // namespace seds
