#pragma once

#include "internal.hpp"

#include <optional>
#include <span>
#include <string_view>

namespace seds {

inline std::span<const TypeInfo> type_info() {
    return {kTypeInfo.data(), kTypeInfo.size()};
}

inline std::span<const char* const> endpoint_names() {
    return {kEndpointNames.data(), kEndpointNames.size()};
}

inline const TypeInfo* find_type_info(uint32_t ty) {
    return valid_type(ty) ? &kTypeInfo[ty] : nullptr;
}

inline std::optional<uint32_t> endpoint_by_name(std::string_view name) {
    for (uint32_t i = 0; i < kEndpointNames.size(); ++i) {
        if (name == kEndpointNames[i]) {
            return i;
        }
    }
    return std::nullopt;
}

}  // namespace seds
