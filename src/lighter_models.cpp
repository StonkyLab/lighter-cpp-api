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
    readValue<std::string>(json, "exchange", exchange);

    // Historical /fundings: rate is an UNSIGNED percent string (e.g. "0.0114" = 0.0114%);
    // divide by 100 for the decimal used by Bybit/Binance, and take the sign from
    // `direction`, which names the side that PAYS: "long" = longs pay → POSITIVE under the
    // cross-exchange convention (positive = longs pay shorts); "short" = shorts pay → negative.
    //
    // This mapping was inverted until 2026-07-31, which silently sign-flipped every seeded
    // history. Verified against the venue's own signed snapshot (/funding-rates, "lighter"
    // rows) on CTR/XLM/TIA/TRUMP — all four disagreed in sign before the fix and agree after.
    // The snapshot's convention is itself confirmed by its "bybit" rows matching Bybit's own
    // API sign-for-sign (XLM to the 8th decimal).
    //
    // Current snapshot /funding-rates: rate is already a signed decimal — but quoted as an
    // 8-HOUR-EQUIVALENT rate (Lighter settles hourly at 1/8 of it). The parser keeps the raw
    // quote; RESTClient::getCurrentFundingRates converts to the hourly settled slice so both
    // funding methods return the same units.
    if (auto it = json.find("rate"); it != json.end() && !it->is_null()) {
        if (it->is_string()) {
            try {
                fundingRate = std::stod(it->get<std::string>()) / 100.0;
                if (direction == "short") fundingRate = -fundingRate;
            } catch (...) { /* leave default */ }
        } else if (it->is_number()) {
            fundingRate = it->get<double>();
        }
    }
}

nlohmann::json Position::toJson() const { throw std::runtime_error("Unimplemented: Position::toJson()"); }

void Position::fromJson(const nlohmann::json& json) {
    readValue<std::int32_t>(json, "market_id", marketId);
    readValue<std::string>(json, "symbol", symbol);
    readValue<int>(json, "sign", sign);
    quantity       = readStringAsDouble(json, "position");
    avgEntryPrice  = readStringAsDouble(json, "avg_entry_price");
    positionValue  = readStringAsDouble(json, "position_value");
    unrealizedPnl  = readStringAsDouble(json, "unrealized_pnl");
    realizedPnl    = readStringAsDouble(json, "realized_pnl");
    liquidationPrice = readStringAsDouble(json, "liquidation_price");
    allocatedMargin  = readStringAsDouble(json, "allocated_margin");
    readValue<int>(json, "margin_mode", marginMode);
}

nlohmann::json AccountAsset::toJson() const { throw std::runtime_error("Unimplemented: AccountAsset::toJson()"); }

void AccountAsset::fromJson(const nlohmann::json& json) {
    readValue<std::string>(json, "symbol", symbol);
    readValue<std::int32_t>(json, "asset_id", assetId);
    balance       = readStringAsDouble(json, "balance");
    lockedBalance = readStringAsDouble(json, "locked_balance");
    marginBalance = readStringAsDouble(json, "margin_balance");
}

nlohmann::json AccountBalance::toJson() const { throw std::runtime_error("Unimplemented: AccountBalance::toJson()"); }

void AccountBalance::fromJson(const nlohmann::json& json) {
    readValue<std::int32_t>(json, "account_index", accountIndex);
    readValue<std::string>(json, "l1_address", l1Address);
    availableBalance              = readStringAsDouble(json, "available_balance");
    collateral                    = readStringAsDouble(json, "collateral");
    totalAssetValue               = readStringAsDouble(json, "total_asset_value");
    crossInitialMarginRequirement = readStringAsDouble(json, "cross_initial_margin_requirement");
    crossMaintenanceMarginRequirement = readStringAsDouble(json, "cross_maintenance_margin_requirement");

    if (auto it = json.find("positions"); it != json.end() && it->is_array()) {
        for (const auto& item : *it) {
            Position pos;
            pos.fromJson(item);
            positions.push_back(pos);
        }
    }
    if (auto it = json.find("assets"); it != json.end() && it->is_array()) {
        for (const auto& item : *it) {
            AccountAsset asset;
            asset.fromJson(item);
            assets.push_back(asset);
        }
    }
}

nlohmann::json SendTxResult::toJson() const { throw std::runtime_error("Unimplemented: SendTxResult::toJson()"); }

void SendTxResult::fromJson(const nlohmann::json& json) {
    readValue<int>(json, "code", code);
    readValue<std::string>(json, "message", message);
    readValue<std::string>(json, "tx_hash", txHash);
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

    if (const auto mc = json.find("market_config"); mc != json.end() && mc->is_object()) {
        int marketMarginMode = 0;
        readValue<int>(*mc, "market_margin_mode", marketMarginMode);
        isolatedOnly = marketMarginMode == 1;
        readValue<bool>(*mc, "force_reduce_only", forceReduceOnly);
    }
}
} // namespace stonky::lighter
