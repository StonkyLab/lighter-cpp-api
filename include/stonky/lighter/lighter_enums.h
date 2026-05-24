/**
Lighter Enums

Licensed under the MIT License <http://opensource.org/licenses/MIT>.
SPDX-License-Identifier: MIT
Copyright (c) 2026 Vitezslav Kot <vitezslav.kot@stonky.cz>, Stonky s.r.o.
*/

#ifndef INCLUDE_STONKY_LIGHTER_ENUMS_H
#define INCLUDE_STONKY_LIGHTER_ENUMS_H

#include "stonky/utils/magic_enum_wrapper.hpp"

namespace stonky::lighter {
enum class CandleInterval : std::int32_t {
    _1m,
    _5m,
    _15m,
    _30m,
    _1h,
    _4h,
    _12h,
    _1d
};
}

template <>
constexpr magic_enum::customize::customize_t magic_enum::customize::enum_name<stonky::lighter::CandleInterval>(
    const stonky::lighter::CandleInterval value) noexcept {
    switch (value) {
    case stonky::lighter::CandleInterval::_1m:
        return "1m";
    case stonky::lighter::CandleInterval::_5m:
        return "5m";
    case stonky::lighter::CandleInterval::_15m:
        return "15m";
    case stonky::lighter::CandleInterval::_30m:
        return "30m";
    case stonky::lighter::CandleInterval::_1h:
        return "1h";
    case stonky::lighter::CandleInterval::_4h:
        return "4h";
    case stonky::lighter::CandleInterval::_12h:
        return "12h";
    case stonky::lighter::CandleInterval::_1d:
        return "1d";
    }
    return default_tag;
}

#endif // INCLUDE_STONKY_LIGHTER_ENUMS_H
