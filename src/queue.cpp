#include "internal.hpp"

namespace seds {
namespace {

template <typename ItemT>
bool enqueue_bounded(std::deque<ItemT>& queue, size_t& queue_bytes, ItemT item, bool front) {
    const size_t cost = byte_cost(item);
    if (cost > kMaxQueueBytes) {
        return false;
    }
    while (!queue.empty() && queue_bytes + cost > kMaxQueueBytes) {
        queue_bytes -= byte_cost(queue.front());
        queue.pop_front();
    }
    if (front) {
        queue.push_front(std::move(item));
    } else {
        queue.push_back(std::move(item));
    }
    queue_bytes += cost;
    return true;
}

}  // namespace

size_t byte_cost(const PacketData& pkt) {
    return sizeof(pkt.ty) + sizeof(pkt.timestamp) + pkt.sender.size() +
           pkt.endpoints.size() * sizeof(uint32_t) + pkt.payload.size();
}

size_t byte_cost(const TxItem& item) {
    return byte_cost(item.pkt) + sizeof(bool) + sizeof(int32_t) * 2u;
}

size_t byte_cost(const RxItem& item) {
    return byte_cost(item.pkt) + sizeof(int32_t) + item.wire_bytes.size();
}

bool enqueue_tx(std::deque<TxItem>& queue, size_t& queue_bytes, TxItem item) {
    return enqueue_bounded(queue, queue_bytes, std::move(item), false);
}

bool enqueue_tx_front(std::deque<TxItem>& queue, size_t& queue_bytes, TxItem item) {
    return enqueue_bounded(queue, queue_bytes, std::move(item), true);
}

bool enqueue_rx(std::deque<RxItem>& queue, size_t& queue_bytes, RxItem item) {
    return enqueue_bounded(queue, queue_bytes, std::move(item), false);
}

std::optional<TxItem> pop_tx(std::deque<TxItem>& queue, size_t& queue_bytes) {
    if (queue.empty()) {
        return std::nullopt;
    }
    queue_bytes -= byte_cost(queue.front());
    TxItem item = std::move(queue.front());
    queue.pop_front();
    return item;
}

std::optional<RxItem> pop_rx(std::deque<RxItem>& queue, size_t& queue_bytes) {
    if (queue.empty()) {
        return std::nullopt;
    }
    queue_bytes -= byte_cost(queue.front());
    RxItem item = std::move(queue.front());
    queue.pop_front();
    return item;
}

void clear_tx_queue(std::deque<TxItem>& queue, size_t& queue_bytes) {
    queue.clear();
    queue_bytes = 0;
}

void clear_rx_queue(std::deque<RxItem>& queue, size_t& queue_bytes) {
    queue.clear();
    queue_bytes = 0;
}

}  // namespace seds
