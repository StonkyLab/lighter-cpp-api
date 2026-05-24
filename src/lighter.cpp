/**
Lighter Common Stuff

Licensed under the MIT License <http://opensource.org/licenses/MIT>.
SPDX-License-Identifier: MIT
Copyright (c) 2026 Vitezslav Kot <vitezslav.kot@stonky.cz>, Stonky s.r.o.
*/
#include "stonky/lighter/lighter.h"

namespace stonky::lighter {
int64_t Lighter::numberOfMsForCandleInterval(const CandleInterval candleInterval) {
    switch (candleInterval) {
        case CandleInterval::_1m:
            return 60000;
        case CandleInterval::_5m:
            return 60000 * 5;
        case CandleInterval::_15m:
            return 60000 * 15;
        case CandleInterval::_30m:
            return 60000 * 30;
        case CandleInterval::_1h:
            return 60000 * 60;
        case CandleInterval::_4h:
            return 60000 * 240;
        case CandleInterval::_12h:
            return 60000 * 720;
        case CandleInterval::_1d:
            return 86400000;
        default:
            return 0;
    }
}

bool Lighter::isValidCandleResolution(const std::int32_t resolution, CandleInterval &candleInterval) {
    switch (resolution) {
        case 1:
            candleInterval = CandleInterval::_1m;
            return true;
        case 5:
            candleInterval = CandleInterval::_5m;
            return true;
        case 15:
            candleInterval = CandleInterval::_15m;
            return true;
        case 30:
            candleInterval = CandleInterval::_30m;
            return true;
        case 60:
            candleInterval = CandleInterval::_1h;
            return true;
        case 240:
            candleInterval = CandleInterval::_4h;
            return true;
        case 720:
            candleInterval = CandleInterval::_12h;
            return true;
        case 1440:
            candleInterval = CandleInterval::_1d;
            return true;
        default:
            return false;
    }
}
}
