#pragma once

#include <optional>
#include <type_traits>

namespace seds {

template <typename EnumT>
constexpr auto to_underlying(EnumT value) noexcept -> std::underlying_type_t<EnumT> {
    static_assert(std::is_enum_v<EnumT>);
    return static_cast<std::underlying_type_t<EnumT>>(value);
}

template <typename EnumT>
constexpr std::optional<EnumT> enum_from_underlying(std::underlying_type_t<EnumT> value,
                                                    std::underlying_type_t<EnumT> min_value,
                                                    std::underlying_type_t<EnumT> max_value) noexcept {
    static_assert(std::is_enum_v<EnumT>);
    if (value < min_value || value > max_value) {
        return std::nullopt;
    }
    return static_cast<EnumT>(value);
}

}  // namespace seds

