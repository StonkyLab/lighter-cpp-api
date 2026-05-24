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
    } catch (const std::exception& e) {
        const std::string msg = e.what();
        spdlog::error("Exception: {}", msg.size() > 200 ? msg.substr(0, 200) + "..." : msg);
        return 1;
    }
    return 0;
}
