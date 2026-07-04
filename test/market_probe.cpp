/**
Read-only Lighter probe: perpetual assets + funding ranking (REST) and a live
order-book BBO over the WebSocket. No account/credentials — validates the
market-data + WS transport the execution gateway depends on.
*/

#include "stonky/lighter/lighter_rest_client.h"
#include "stonky/lighter/lighter_ws_stream_manager.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>

using namespace stonky::lighter;

int main() {
    spdlog::set_level(spdlog::level::info);

    try {
        const RESTClient rest;

        const auto assets = rest.getPerpetualAssets(false);
        spdlog::info("perpetual assets: {}", assets.size());

        int probeMarketId = -1;
        std::string probeSymbol;
        for (const auto& a: assets) {
            if (a.symbol == "ETH") {
                probeMarketId = a.marketId;
                probeSymbol = a.symbol;
                spdlog::info("  {} marketId={} sizeDec={} priceDec={} minBase={}", a.symbol, a.marketId, a.sizeDecimals, a.priceDecimals, a.minBaseAmount);
            }
        }

        auto ranked = rest.getCurrentFundingRates();
        std::ranges::sort(ranked, [](const auto& x, const auto& y) { return x.fundingRate < y.fundingRate; });
        spdlog::info("funding candidates: {}", ranked.size());
        spdlog::info("--- most NEGATIVE (would LONG) ---");
        for (std::size_t i = 0; i < std::min<std::size_t>(5, ranked.size()); ++i) {
            spdlog::info("  {} {:+.4f}%", ranked[i].symbol, ranked[i].fundingRate * 100.0);
        }
        spdlog::info("--- most POSITIVE (would SHORT) ---");
        for (std::size_t i = 0; i < std::min<std::size_t>(5, ranked.size()); ++i) {
            const auto& e = ranked[ranked.size() - 1 - i];
            spdlog::info("  {} {:+.4f}%", e.symbol, e.fundingRate * 100.0);
        }

        if (probeMarketId < 0) {
            spdlog::warn("ETH market not found — skipping WS probe");
            return 0;
        }

        spdlog::info("=== WS order book probe: {} (marketId {}) ===", probeSymbol, probeMarketId);
        WSStreamManager stream;
        std::atomic<int> ticks{0};
        stream.setOrderBookCallback([&](const int marketId, const double bid, const double ask) {
            if (ticks.fetch_add(1) < 3) spdlog::info("  BBO {} bid={} ask={}", marketId, bid, ask);
        });
        stream.start();
        stream.subscribeOrderBook(probeMarketId);

        for (int i = 0; i < 60 && ticks.load() < 3; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        spdlog::info("order-book ticks received: {}", ticks.load());
    } catch (std::exception& e) {
        spdlog::critical("probe exception: {}", e.what());
        return 1;
    }

    return 0;
}
