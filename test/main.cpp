#include "stonky/lighter/lighter_rest_client.h"
#include "stonky/lighter/lighter.h"
#include <chrono>
#include <thread>
#include <spdlog/spdlog.h>

using namespace stonky::lighter;

void testCandlesOnly() {
    const auto restClient = std::make_unique<RESTClient>();
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto fromMs = nowMs - 86400000LL;
    // Skip /orderBookDetails — call /candles cold with known BTC market_id=1
    const auto candles = restClient->getHistoricalPrices(static_cast<std::int32_t>(1),
                                                          CandleInterval::_1h, fromMs, nowMs);
    spdlog::info("Downloaded {} BTC 1h candles", candles.size());
    if (!candles.empty()) {
        const auto& last = candles.back();
        spdlog::info("Last: t={}, o={}, h={}, l={}, c={}", last.openTime, last.open, last.high, last.low, last.close);
    }
}

void testPerpAssets() {
    const auto restClient = std::make_unique<RESTClient>();
    const auto assets = restClient->getPerpetualAssets();
    spdlog::info("Downloaded {} active Lighter perpetual markets", assets.size());
    for (size_t i = 0; i < std::min<size_t>(5, assets.size()); ++i) {
        const auto& a = assets[i];
        spdlog::info("  [{}] {} (market_id={}, taker_fee={})", i, a.symbol, a.marketId, a.takerFee);
    }
}

void testHistoricalFunding() {
    const auto restClient = std::make_unique<RESTClient>();
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto fromMs = nowMs - 86400000LL;
    const auto rates = restClient->getFundingRates(static_cast<std::int32_t>(1), fromMs, nowMs);
    spdlog::info("Downloaded {} BTC funding events", rates.size());
    if (!rates.empty()) {
        const auto& last = rates.back();
        spdlog::info("Last: marketId={}, t={}, rate={}", last.marketId, last.fundingTime, last.fundingRate);
    }
}

void testCurrentFunding() {
    const auto restClient = std::make_unique<RESTClient>();
    const auto rates = restClient->getCurrentFundingRates();
    spdlog::info("Current funding snapshot: {} Lighter markets", rates.size());
    for (size_t i = 0; i < std::min<size_t>(3, rates.size()); ++i) {
        const auto& fr = rates[i];
        spdlog::info("  [{}] {} (market_id={}) rate={}", i, fr.symbol, fr.marketId, fr.fundingRate);
    }
}

void testBalance() {
    const auto restClient = std::make_unique<RESTClient>();
    // Account index 0 is a public Lighter account with assets; safe for unauthenticated probing.
    const auto bal = restClient->getBalance(0);
    spdlog::info("Account index={} l1={}", bal.accountIndex, bal.l1Address);
    spdlog::info("  availableBalance={:.6f}  collateral={:.6f}  totalAssetValue={:.6f}",
                 bal.availableBalance, bal.collateral, bal.totalAssetValue);
    spdlog::info("  crossIM={:.6f}  crossMM={:.6f}",
                 bal.crossInitialMarginRequirement, bal.crossMaintenanceMarginRequirement);
    spdlog::info("  positions={}", bal.positions.size());
    for (const auto& p : bal.positions) {
        spdlog::info("    {} (market_id={}) sign={} qty={} entry={} uPnL={} margin={}",
                     p.symbol, p.marketId, p.sign, p.quantity,
                     p.avgEntryPrice, p.unrealizedPnl, p.allocatedMargin);
    }
    spdlog::info("  assets={}", bal.assets.size());
    for (size_t i = 0; i < std::min<size_t>(3, bal.assets.size()); ++i) {
        const auto& a = bal.assets[i];
        spdlog::info("    {} (id={}) balance={:.6f} locked={:.6f} margin={:.6f}",
                     a.symbol, a.assetId, a.balance, a.lockedBalance, a.marginBalance);
    }
}

int main() {
    try {
        spdlog::info("--- testCandlesOnly (cold) ---");
        testCandlesOnly();
        std::this_thread::sleep_for(std::chrono::seconds(2));

        spdlog::info("--- testPerpAssets ---");
        testPerpAssets();
        std::this_thread::sleep_for(std::chrono::seconds(2));

        spdlog::info("--- testHistoricalFunding ---");
        testHistoricalFunding();
        std::this_thread::sleep_for(std::chrono::seconds(2));

        spdlog::info("--- testCurrentFunding ---");
        testCurrentFunding();
        std::this_thread::sleep_for(std::chrono::seconds(2));

        spdlog::info("--- testBalance ---");
        testBalance();
    } catch (const std::exception& e) {
        const std::string msg = e.what();
        spdlog::error("Exception: {}", msg.size() > 200 ? msg.substr(0, 200) + "..." : msg);
        return 1;
    }
    return 0;
}
