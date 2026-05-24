/**
Lighter REST Client

Licensed under the MIT License <http://opensource.org/licenses/MIT>.
SPDX-License-Identifier: MIT
Copyright (c) 2026 Vitezslav Kot <vitezslav.kot@stonky.cz>, Stonky s.r.o.
*/

#include "stonky/lighter/lighter_rest_client.h"
#include "stonky/lighter/lighter_http_session.h"
#include "stonky/lighter/lighter.h"
#include "stonky/utils/json_utils.h"
#include <spdlog/spdlog.h>
#include <unordered_map>

namespace stonky::lighter {

namespace {
constexpr int kMaxBarsPerCall = 500;
constexpr const char* kCandlesPath = "/api/v1/candles";
constexpr const char* kFundingsPath = "/api/v1/fundings";
constexpr const char* kFundingRatesPath = "/api/v1/funding-rates";
constexpr const char* kOrderBookDetailsPath = "/api/v1/orderBookDetails";
constexpr const char* kLighterExchangeTag = "lighter";
constexpr const char* kFundingResolution = "1h"; // Lighter funding period
} // namespace

struct RESTClient::P {
    std::shared_ptr<HTTPSession> httpSession;
    std::unordered_map<std::string, std::int32_t> symbolToMarketId;
    bool symbolCachePopulated{false};

    http::response<http::string_body> checkTransport(const http::response<http::string_body>& response) const {
        if (response.result() != http::status::ok) {
            throw std::runtime_error(
                fmt::format("Lighter HTTP error, code {}, msg: {}", response.result_int(), response.body()));
        }
        return response;
    }

    nlohmann::json parseEnvelope(const http::response<http::string_body>& response) const {
        const auto json = nlohmann::json::parse(response.body());
        // Lighter wraps every response in {code, message?, ...}. code==200 means OK.
        if (auto it = json.find("code"); it != json.end() && it->is_number_integer()) {
            if (it->get<int>() != 200) {
                std::string msg;
                readValue<std::string>(json, "message", msg);
                throw std::runtime_error(fmt::format("Lighter API error, code {}, msg: {}", it->get<int>(), msg));
            }
        }
        return json;
    }

    void ensureSymbolCache() {
        if (symbolCachePopulated) return;
        loadAssets(true); // include delisted in cache for accurate lookup
    }

    std::vector<PerpAsset> loadAssets(bool populateCache) {
        const auto response = checkTransport(httpSession->get(kOrderBookDetailsPath, nlohmann::json::object()));
        const auto json = parseEnvelope(response);

        std::vector<PerpAsset> assets;
        if (auto it = json.find("order_book_details"); it != json.end() && it->is_array()) {
            for (const auto& item : *it) {
                PerpAsset asset;
                asset.fromJson(item);
                assets.push_back(asset);
            }
        }

        if (populateCache) {
            symbolToMarketId.clear();
            for (const auto& a : assets) {
                symbolToMarketId[a.symbol] = a.marketId;
            }
            symbolCachePopulated = true;
        }
        return assets;
    }

    std::int32_t resolveMarketId(const std::string& symbol) {
        ensureSymbolCache();
        if (auto it = symbolToMarketId.find(symbol); it != symbolToMarketId.end()) {
            return it->second;
        }
        // Refresh-on-miss in case a new market appeared mid-session
        loadAssets(true);
        if (auto it = symbolToMarketId.find(symbol); it != symbolToMarketId.end()) {
            return it->second;
        }
        throw std::runtime_error(fmt::format("Lighter: unknown symbol '{}'", symbol));
    }

    std::vector<Candle> getCandlesBatch(std::int32_t marketId, CandleInterval interval,
                                         std::int64_t fromSec, std::int64_t toSec) const {
        nlohmann::json q;
        q["market_id"] = marketId;
        q["resolution"] = magic_enum::enum_name(interval);
        q["start_timestamp"] = fromSec;
        q["end_timestamp"] = toSec;
        q["count_back"] = kMaxBarsPerCall;

        const auto response = checkTransport(httpSession->get(kCandlesPath, q));
        const auto json = parseEnvelope(response);

        std::vector<Candle> candles;
        if (auto it = json.find("c"); it != json.end() && it->is_array()) {
            for (const auto& item : *it) {
                Candle candle;
                candle.fromJson(item);
                candles.push_back(candle);
            }
        }
        return candles;
    }

    std::vector<FundingRate> getFundingBatch(std::int32_t marketId, std::int64_t fromSec, std::int64_t toSec) const {
        nlohmann::json q;
        q["market_id"] = marketId;
        q["resolution"] = kFundingResolution;
        q["start_timestamp"] = fromSec;
        q["end_timestamp"] = toSec;
        q["count_back"] = kMaxBarsPerCall;

        const auto response = checkTransport(httpSession->get(kFundingsPath, q));
        const auto json = parseEnvelope(response);

        std::vector<FundingRate> rates;
        if (auto it = json.find("fundings"); it != json.end() && it->is_array()) {
            for (const auto& item : *it) {
                FundingRate fr;
                fr.fromJson(item);
                // Historical payload has no market_id/symbol; backfill from query context
                if (fr.marketId < 0) fr.marketId = marketId;
                rates.push_back(fr);
            }
        }
        return rates;
    }
};

RESTClient::RESTClient() : m_p(std::make_unique<P>()) { m_p->httpSession = std::make_shared<HTTPSession>(); }

RESTClient::~RESTClient() = default;

std::vector<PerpAsset> RESTClient::getPerpetualAssets(const bool includeDelisted) const {
    auto assets = m_p->loadAssets(true);
    if (!includeDelisted) {
        std::erase_if(assets, [](const PerpAsset& a) { return a.isDelisted; });
    }
    return assets;
}

std::vector<Candle> RESTClient::getHistoricalPrices(const std::string& symbol, const CandleInterval interval,
                                                    const std::int64_t from, const std::int64_t to,
                                                    const onCandlesDownloaded& writer) const {
    return getHistoricalPrices(m_p->resolveMarketId(symbol), interval, from, to, writer);
}

std::vector<Candle> RESTClient::getHistoricalPrices(const std::int32_t marketId, const CandleInterval interval,
                                                    const std::int64_t from, const std::int64_t to,
                                                    const onCandlesDownloaded& writer) const {
    const std::int64_t intervalMs = Lighter::numberOfMsForCandleInterval(interval);
    if (intervalMs <= 0) {
        throw std::runtime_error("Lighter: invalid candle interval");
    }
    const std::int64_t intervalSec = intervalMs / 1000;
    const std::int64_t windowSec = intervalSec * kMaxBarsPerCall;

    std::vector<Candle> retVal;
    std::int64_t batchFromSec = from / 1000;
    const std::int64_t toSec = to / 1000;

    while (batchFromSec < toSec) {
        const std::int64_t batchToSec = std::min(batchFromSec + windowSec, toSec);
        auto candles = m_p->getCandlesBatch(marketId, interval, batchFromSec, batchToSec);

        if (candles.empty()) break;

        // Drop the last candle if it is still open (its end would exceed 'to')
        if (candles.back().openTime + intervalMs > to) {
            candles.pop_back();
        }
        if (candles.empty()) break;

        if (writer) writer(candles);

        retVal.insert(retVal.end(), candles.begin(), candles.end());
        batchFromSec = (candles.back().openTime + intervalMs) / 1000;
    }

    return retVal;
}

std::vector<FundingRate> RESTClient::getFundingRates(const std::string& symbol, const std::int64_t startTime,
                                                      const std::int64_t endTime) const {
    return getFundingRates(m_p->resolveMarketId(symbol), startTime, endTime);
}

std::vector<FundingRate> RESTClient::getFundingRates(const std::int32_t marketId, const std::int64_t startTime,
                                                      const std::int64_t endTime) const {
    // Lighter funding is hourly; paginate in 500-event windows just like candles
    constexpr std::int64_t fundingIntervalMs = 60 * 60 * 1000;
    constexpr std::int64_t windowMs = fundingIntervalMs * kMaxBarsPerCall;

    std::vector<FundingRate> retVal;
    std::int64_t batchFromMs = startTime;

    while (batchFromMs < endTime) {
        const std::int64_t batchToMs = std::min(batchFromMs + windowMs, endTime);
        auto batch = m_p->getFundingBatch(marketId, batchFromMs / 1000, batchToMs / 1000);

        if (batch.empty()) break;

        // Lighter funding timestamps may be in seconds or ms — normalise to ms.
        for (auto& fr : batch) {
            if (fr.fundingTime > 0 && fr.fundingTime < 100000000000LL) {
                fr.fundingTime *= 1000; // looked like seconds (<~year 5138), convert to ms
            }
        }

        if (batch.back().fundingTime > endTime) {
            batch.pop_back();
        }
        if (batch.empty()) break;

        retVal.insert(retVal.end(), batch.begin(), batch.end());
        batchFromMs = batch.back().fundingTime + fundingIntervalMs;
    }
    return retVal;
}

std::vector<FundingRate> RESTClient::getCurrentFundingRates() const {
    const auto response = m_p->checkTransport(m_p->httpSession->get(kFundingRatesPath, nlohmann::json::object()));
    const auto json = m_p->parseEnvelope(response);

    std::vector<FundingRate> rates;
    if (auto it = json.find("funding_rates"); it != json.end() && it->is_array()) {
        for (const auto& item : *it) {
            // Endpoint aggregates rates across multiple exchanges (binance/bybit/hyperliquid/lighter).
            // We only care about Lighter's own values.
            std::string exchange;
            readValue<std::string>(item, "exchange", exchange);
            if (exchange != kLighterExchangeTag) continue;

            FundingRate fr;
            fr.fromJson(item);
            rates.push_back(fr);
        }
    }
    return rates;
}

} // namespace stonky::lighter
