/**
Lighter Data Models

Licensed under the MIT License <http://opensource.org/licenses/MIT>.
SPDX-License-Identifier: MIT
Copyright (c) 2026 Vitezslav Kot <vitezslav.kot@stonky.cz>, Stonky s.r.o.
*/

#ifndef LIGHTER_API_LIGHTER_MODELS_H
#define LIGHTER_API_LIGHTER_MODELS_H

#include "stonky/lighter/lighter_enums.h"
#include "stonky/interface/i_json.h"
#include <nlohmann/json.hpp>
#include <vector>
#include <string>

namespace stonky::lighter {

struct Candle final : IJson {
    std::int64_t openTime{}; // wire: "t"
    double open{};           // wire: "o"
    double high{};           // wire: "h"
    double low{};            // wire: "l"
    double close{};          // wire: "c"
    double baseVolume{};     // wire: "v"
    double quoteVolume{};    // wire: "V"

    [[nodiscard]] nlohmann::json toJson() const override;

    void fromJson(const nlohmann::json& json) override;
};

struct FundingRate final : IJson {
    std::int32_t marketId{-1};
    std::string symbol{};
    double fundingRate{};
    std::int64_t fundingTime{}; // only set on historical query; 0 on current snapshot
    std::string direction{};    // only set on historical query; empty on current snapshot
    /// Only set on the current snapshot (/funding-rates), which publishes rows
    /// for MULTIPLE venues ("lighter", "binance", "bybit", "hyperliquid").
    /// A consumer that wants LIGHTER's own funding MUST filter on this.
    std::string exchange{};

    [[nodiscard]] nlohmann::json toJson() const override;

    void fromJson(const nlohmann::json& json) override;
};

struct Position final : IJson {
    std::int32_t marketId{-1};
    std::string symbol{};
    int sign{0};             // 1 = long, -1 = short
    double quantity{};       // wire: "position"
    double avgEntryPrice{};
    double positionValue{};  // notional in USDC
    double unrealizedPnl{};
    double realizedPnl{};
    double liquidationPrice{};
    double allocatedMargin{};
    int marginMode{0};       // wire: "margin_mode" — 0 = cross, 1 = isolated

    [[nodiscard]] nlohmann::json toJson() const override;

    void fromJson(const nlohmann::json& json) override;
};

struct AccountAsset final : IJson {
    std::string symbol{};
    std::int32_t assetId{-1};
    double balance{};
    double lockedBalance{};
    double marginBalance{};

    [[nodiscard]] nlohmann::json toJson() const override;

    void fromJson(const nlohmann::json& json) override;
};

struct AccountBalance final : IJson {
    std::int32_t accountIndex{-1};
    std::string l1Address{};
    double availableBalance{};              // free USDC for new orders
    double collateral{};                    // total collateral (including locked)
    double totalAssetValue{};               // total portfolio value in USDC
    double crossInitialMarginRequirement{};
    double crossMaintenanceMarginRequirement{};
    std::vector<Position> positions{};      // only non-flat when queried with active_only=true
    std::vector<AccountAsset> assets{};     // spot / collateral holdings

    [[nodiscard]] nlohmann::json toJson() const override;

    void fromJson(const nlohmann::json& json) override;
};

struct SendTxResult final : IJson {
    int code{};             // 200 = accepted
    std::string message{};  // error message when code != 200
    std::string txHash{};   // venue tx hash

    [[nodiscard]] nlohmann::json toJson() const override;

    void fromJson(const nlohmann::json& json) override;
};

struct PerpAsset final : IJson {
    std::string symbol{};
    std::int32_t marketId{-1};
    /// True when the MARKET itself is isolated-margin-only
    /// (market_config.market_margin_mode == 1) — a venue-side property of 9
    /// markets as of 2026-07-31 (FOLKS, H100, OPENAI, ...). No account action
    /// (UI or API) can make such a market cross-margin; a delta-neutral pair
    /// strategy must exclude these at the universe level, because an isolated
    /// leg can be liquidated on its own margin while its hedge lives on.
    bool isolatedOnly{false};
    std::string marketType{};
    std::string status{};
    int sizeDecimals{};
    int priceDecimals{};
    double takerFee{};
    double makerFee{};
    double minBaseAmount{};
    double minQuoteAmount{};
    bool isDelisted{false}; // derived: true unless status == "active"

    [[nodiscard]] nlohmann::json toJson() const override;

    void fromJson(const nlohmann::json& json) override;
};
} // namespace stonky::lighter

#endif // LIGHTER_API_LIGHTER_MODELS_H
