/**
Lighter Common Stuff

Licensed under the MIT License <http://opensource.org/licenses/MIT>.
SPDX-License-Identifier: MIT
Copyright (c) 2026 Vitezslav Kot <vitezslav.kot@stonky.cz>, Stonky s.r.o.
*/

#ifndef INCLUDE_STONKY_LIGHTER_H
#define INCLUDE_STONKY_LIGHTER_H

#include "lighter_models.h"

namespace stonky::lighter {
class Lighter {
public:
    /**
     * Check if the input resolution in minutes is valid, if so then return corresponding API string
     * @param resolution Candle resolution in minutes.
     * @param candleInterval out: CandleInterval enum value
     * @return True if input resolution is valid for Lighter
     */
    static bool isValidCandleResolution(std::int32_t resolution, CandleInterval &candleInterval);

    /**
     * Get a number of ms for a given candle interval
     * @param candleInterval
     * @return
     */
    static int64_t numberOfMsForCandleInterval(CandleInterval candleInterval);
};
}
#endif // INCLUDE_STONKY_LIGHTER_H
