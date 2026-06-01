/**
Lighter Data Models

Licensed under the MIT License <http://opensource.org/licenses/MIT>.
SPDX-License-Identifier: MIT
Copyright (c) 2026 Vitezslav Kot <vitezslav.kot@stonky.cz>, Stonky s.r.o.
*/

#include "stonky/lighter/lighter_models.h"
#include "stonky/utils/json_utils.h"

namespace stonky::lighter {

nlohmann::json Candle::toJson() const { throw std::runtime_error("Unimplemented: Candle::toJson()"); }

void Candle::fromJson(const nlohmann::json& json) {
    readValue<std::int64_t>(json, "t", openTime);
    // OHLCV come as numbers on /candles (not strings, unlike Hyperliquid)
    readValue<double>(json, "o", open);
    readValue<double>(json, "h", high);
    readValue<double>(json, "l", low);
    readValue<double>(json, "c", close);
    readValue<double>(json, "v", baseVolume);
    readValue<double>(json, "V", quoteVolume);
}

nlohmann::json FundingRate::toJson() const { throw std::runtime_error("Unimplemented: FundingRate::toJson()"); }

void FundingRate::fromJson(const nlohmann::json& json) {
    // Historical (/fundings) carries timestamp + value + rate(string) + direction.
    // Current snapshot (/funding-rates) carries market_id + symbol + exchange + rate(number).
    readValue<std::int64_t>(json, "timestamp", fundingTime);
    readValue<std::string>(json, "direction", direction);
    readValue<std::int32_t>(json, "market_id", marketId);
    readValue<std::string>(json, "symbol", symbol);

    // rate is either string (historical) or number (current). Dispatch on type.
    if (auto it = json.find("rate"); it != json.end() && !it->is_null()) {
        if (it->is_string()) {
            try { fundingRate = std::stod(it->get<std::string>()); } catch (...) { /* leave default */ }
        } else if (it->is_number()) {
            fundingRate = it->get<double>();
        }
    }

    // Historical /fundings encodes the rate as a positive magnitude + a
    // separate `direction` string ("long" or "short"). Standard cross-exchange
    // convention is signed funding (positive = longs pay shorts, negative =
    // shorts pay longs), so apply the sign from `direction` here. Current
    // snapshot endpoint has no `direction` field so this is a no-op there.
    if (direction == "short" && fundingRate > 0.0) {
        fundingRate = -fundingRate;
    }
}

nlohmann::json PerpAsset::toJson() const { throw std::runtime_error("Unimplemented: PerpAsset::toJson()"); }

void PerpAsset::fromJson(const nlohmann::json& json) {
    readValue<std::string>(json, "symbol", symbol);
    readValue<std::int32_t>(json, "market_id", marketId);
    readValue<std::string>(json, "market_type", marketType);
    readValue<std::string>(json, "status", status);
    readValue<int>(json, "supported_size_decimals", sizeDecimals);
    readValue<int>(json, "supported_price_decimals", priceDecimals);
    takerFee = readStringAsDouble(json, "taker_fee", takerFee);
    makerFee = readStringAsDouble(json, "maker_fee", makerFee);
    minBaseAmount = readStringAsDouble(json, "min_base_amount", minBaseAmount);
    minQuoteAmount = readStringAsDouble(json, "min_quote_amount", minQuoteAmount);
    isDelisted = status != "active";
}
} // namespace stonky::lighter
