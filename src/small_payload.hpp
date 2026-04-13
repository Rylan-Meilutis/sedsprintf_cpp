#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace seds {

class SmallPayload {
public:
    static constexpr size_t kInlineBytes = 64;

    SmallPayload() = default;
    explicit SmallPayload(std::span<const uint8_t> bytes) { assign(bytes); }

    void assign(std::span<const uint8_t> bytes) {
        size_ = bytes.size();
        if (bytes.size() <= inline_.size()) {
            std::ranges::copy(bytes, inline_.begin());
            heap_.clear();
            heap_.shrink_to_fit();
            using_heap_ = false;
            return;
        }
        heap_.assign(bytes.begin(), bytes.end());
        using_heap_ = true;
    }

    [[nodiscard]] std::span<const uint8_t> bytes() const {
        if (using_heap_) {
            return heap_;
        }
        return {inline_.data(), size_};
    }

    [[nodiscard]] bool using_heap() const { return using_heap_; }
    [[nodiscard]] size_t size() const { return size_; }

private:
    size_t size_{0};
    bool using_heap_{false};
    std::array<uint8_t, kInlineBytes> inline_{};
    std::vector<uint8_t> heap_;
};

}  // namespace seds
