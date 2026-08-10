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
#include <chrono>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace stonky::lighter {

namespace {
constexpr int kMaxBarsPerCall = 500;
constexpr const char* kCandlesPath = "/api/v1/candles";
constexpr const char* kFundingsPath = "/api/v1/fundings";
constexpr const char* kFundingRatesPath = "/api/v1/funding-rates";
constexpr const char* kOrderBookDetailsPath = "/api/v1/orderBookDetails";
constexpr const char* kLighterExchangeTag = "lighter";
constexpr const char* kAccountPath = "/api/v1/account";
constexpr const char* kFundingResolution = "1h"; // Lighter funding period
constexpr const char* kNextNoncePath = "/api/v1/nextNonce";
constexpr const char* kSendTxPath = "/api/v1/sendTx";

/// Percent-encode a value for an application/x-www-form-urlencoded body
/// (RFC 3986 unreserved set kept literal).
std::string urlEncode(const std::string& s) {
    std::string out;
    out.reserve(s.size() * 3);
    for (const unsigned char c : s) {
        if (std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out += fmt::format("%{:02X}", c);
        }
    }
    return out;
}
} // namespace

struct RESTClient::P {
    std::shared_ptr<HTTPSession> httpSession;
    std::unordered_map<std::string, std::int32_t> symbolToMarketId;
    bool symbolCachePopulated{false};

    // Optional Lighter read-only auth token. When non-empty it is sent as the
    // Authorization header on every request, bypassing IP-based rate limits and
    // applying the L1-address quota for the account's tier.
    std::string authToken{};

    // Minimum interval between HTTP requests. Default (1000 ms) matches the
    // Standard tier limit of 60 weighted req/min. Authenticated clients pass a
    // smaller value derived from their tier (Premium 750 ms, Plus 150 ms,
    // Builder 75 ms) so the throttle stays just under the documented ceiling.
    // Exceeding the tier limit returns HTTP 429 wrapped as
    // {"code":23000,"message":"Too Many Requests!"} and triggers a 60-second
    // firewall cooldown — far costlier than pacing requests in the first place.
    // See https://apidocs.lighter.xyz/docs/rate-limits
    std::chrono::milliseconds minRequestInterval{1000};
    mutable std::mutex throttleMtx;
    mutable std::chrono::steady_clock::time_point lastRequestAt{};

    void throttle() const {
        std::lock_guard<std::mutex> lock(throttleMtx);
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = now - lastRequestAt;
        if (elapsed < minRequestInterval) {
            std::this_thread::sleep_for(minRequestInterval - elapsed);
        }
        lastRequestAt = std::chrono::steady_clock::now();
    }

    http::response<http::string_body> checkTransport(const http::response<http::string_body>& response) const {
        if (response.result() != http::status::ok) {
            throw std::runtime_error(
                fmt::format("Lighter HTTP error, code {}, msg: {}", response.result_int(), response.body()));
        }
        return response;
    }

    nlohmann::json parseEnvelope(const http::response<http::string_body>& response) const {
        const auto json = nlohmann::json::parse(response.body());
        if (!json.is_object()) {
            throw std::runtime_error("Lighter API returned a non-object response envelope");
        }
        // Lighter wraps every response in {code, message?, ...}. code==200 means OK.
        const auto code = json.find("code");
        if (code == json.end() || !code->is_number_integer()) {
            throw std::runtime_error("Lighter API response is missing integer field 'code'");
        }
        if (code->get<int>() != 200) {
            std::string msg;
            readValue<std::string>(json, "message", msg);
            throw std::runtime_error(fmt::format("Lighter API error, code {}, msg: {}", code->get<int>(), msg));
        }
        return json;
    }

    void ensureSymbolCache() {
        if (symbolCachePopulated) return;
        loadAssets(true); // include delisted in cache for accurate lookup
    }

    std::vector<PerpAsset> loadAssets(bool populateCache) {
        throttle();
        const auto response = checkTransport(httpSession->get(kOrderBookDetailsPath, nlohmann::json::object(), authToken));
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
                                         std::int64_t startSec, std::int64_t endSec,
                                         std::int64_t countBack) const {
        nlohmann::json q;
        q["market_id"] = marketId;
        q["resolution"] = magic_enum::enum_name(interval);
        q["start_timestamp"] = startSec;
        q["end_timestamp"] = endSec;
        q["count_back"] = countBack;

        throttle();
        const auto response = checkTransport(httpSession->get(kCandlesPath, q, authToken));
        const auto json = parseEnvelope(response);

        const auto candleData = json.find("c");
        if (candleData == json.end() || !candleData->is_array()) {
            throw std::runtime_error("Lighter candle response is missing array field 'c'");
        }

        std::vector<Candle> candles;
        for (const auto& item : *candleData) {
            Candle candle;
            candle.fromJson(item);
            candles.push_back(candle);
        }
        return candles;
    }

    AccountBalance getAccountBalance(const std::string& byKey, const std::string& byValue,
                                      bool activeOnly) const {
        nlohmann::json q;
        q["by"] = byKey;
        q["value"] = byValue;
        if (activeOnly) q["active_only"] = true;

        throttle();
        const auto response = checkTransport(httpSession->get(kAccountPath, q, authToken));
        const auto json = parseEnvelope(response);

        auto it = json.find("accounts");
        if (it == json.end() || !it->is_array() || it->empty()) {
            throw std::runtime_error(fmt::format("Lighter: no account found for {}={}", byKey, byValue));
        }
        AccountBalance balance;
        balance.fromJson(it->front());
        return balance;
    }

    std::vector<FundingRate> getFundingBatch(std::int32_t marketId, std::int64_t startSec, std::int64_t endSec,
                                              std::int64_t countBack) const {
        nlohmann::json q;
        q["market_id"] = marketId;
        q["resolution"] = kFundingResolution;
        q["start_timestamp"] = startSec;
        q["end_timestamp"] = endSec;
        q["count_back"] = countBack;

        throttle();
        const auto response = checkTransport(httpSession->get(kFundingsPath, q, authToken));
        const auto json = parseEnvelope(response);

        const auto fundingData = json.find("fundings");
        if (fundingData == json.end() || !fundingData->is_array()) {
            throw std::runtime_error("Lighter funding response is missing array field 'fundings'");
        }

        std::vector<FundingRate> rates;
        for (const auto& item : *fundingData) {
            FundingRate fr;
            fr.fromJson(item);
            // Historical payload has no market_id/symbol; backfill from query context
            if (fr.marketId < 0) fr.marketId = marketId;
            rates.push_back(fr);
        }
        return rates;
    }
};

RESTClient::RESTClient() : m_p(std::make_unique<P>()) { m_p->httpSession = std::make_shared<HTTPSession>(); }

RESTClient::RESTClient(std::string authToken, std::chrono::milliseconds minRequestInterval)
    : m_p(std::make_unique<P>()) {
    m_p->httpSession = std::make_shared<HTTPSession>();
    m_p->authToken = std::move(authToken);
    if (minRequestInterval.count() > 0) {
        m_p->minRequestInterval = minRequestInterval;
    }
}

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

    // Lighter /candles follows the TradingView-UDF convention: `count_back` is authoritative
    // and OVERRIDES `start_timestamp` — the API always returns the last `count_back` candles
    // ending at `end_timestamp`, ignoring `start_timestamp`. Advancing `start_timestamp`
    // (the obvious approach) therefore never moves the window: every batch returns the same
    // most-recent candles, the cursor never advances, and the loop spins forever, flooding
    // the endpoint until the AWS WAF rate limiter answers HTTP 405. Instead we paginate by
    // walking `end_timestamp` FORWARD, sizing `count_back` to the window so each batch holds
    // <= kMaxBarsPerCall candles. windowSec spans (kMaxBarsPerCall - 1) intervals so the
    // inclusive count_back never exceeds kMaxBarsPerCall.
    const std::int64_t windowSec = intervalSec * (kMaxBarsPerCall - 1);

    std::vector<Candle> retVal;
    const std::int64_t toSec = to / 1000;
    std::int64_t cursorSec = from / 1000;
    std::int64_t lastOpenTime = -1; // ms; dedupe candles that overlap across window boundaries

    while (cursorSec < toSec) {
        const std::int64_t batchEndSec = std::min(cursorSec + windowSec, toSec);
        std::int64_t countBack = (batchEndSec - cursorSec) / intervalSec + 1;
        if (countBack > kMaxBarsPerCall) countBack = kMaxBarsPerCall;

        const auto batch = m_p->getCandlesBatch(marketId, interval, cursorSec, batchEndSec, countBack);

        std::vector<Candle> kept;
        for (const auto& c : batch) {
            if (c.openTime < from) continue;            // count_back may over-fetch before 'from'
            if (c.openTime + intervalMs > to) continue; // drop still-open / out-of-range tail
            if (c.openTime <= lastOpenTime) continue;   // already taken in a previous window
            kept.push_back(c);
            lastOpenTime = c.openTime;
        }

        if (!kept.empty()) {
            if (writer) writer(kept);
            retVal.insert(retVal.end(), kept.begin(), kept.end());
        }

        cursorSec = batchEndSec; // strictly increases each iteration -> guaranteed termination
    }

    return retVal;
}

std::vector<FundingRate> RESTClient::getFundingRates(const std::string& symbol, const std::int64_t startTime,
                                                      const std::int64_t endTime) const {
    return getFundingRates(m_p->resolveMarketId(symbol), startTime, endTime);
}

std::vector<FundingRate> RESTClient::getFundingRates(const std::int32_t marketId, const std::int64_t startTime,
                                                      const std::int64_t endTime) const {
    // Lighter funding is hourly. /fundings shares the candle endpoint's UDF quirk: `count_back`
    // is a *minimum* number of events ending at `end_timestamp`, so a fixed count_back=500 over a
    // short window over-fetches and returns events from BEFORE `startTime` (e.g. a 24h request
    // yields the last 500h). We therefore paginate the same way as candles — walk `end_timestamp`
    // forward, size `count_back` to the window, and filter to [startTime, endTime] — so the
    // returned set matches the requested range.
    constexpr std::int64_t fundingIntervalSec = 60 * 60;
    constexpr std::int64_t windowSec = fundingIntervalSec * (kMaxBarsPerCall - 1);

    std::vector<FundingRate> retVal;
    const std::int64_t toSec = endTime / 1000;
    std::int64_t cursorSec = startTime / 1000;
    std::int64_t lastTime = -1; // ms; dedupe events that overlap across window boundaries

    while (cursorSec < toSec) {
        const std::int64_t batchEndSec = std::min(cursorSec + windowSec, toSec);
        std::int64_t countBack = (batchEndSec - cursorSec) / fundingIntervalSec + 1;
        if (countBack > kMaxBarsPerCall) countBack = kMaxBarsPerCall;

        auto batch = m_p->getFundingBatch(marketId, cursorSec, batchEndSec, countBack);

        for (auto& fr : batch) {
            // Lighter funding timestamps may arrive in seconds — normalise to ms.
            if (fr.fundingTime > 0 && fr.fundingTime < 100000000000LL) {
                fr.fundingTime *= 1000; // looked like seconds (<~year 5138), convert to ms
            }
        }

        for (const auto& fr : batch) {
            if (fr.fundingTime < startTime) continue;  // count_back may over-fetch before 'from'
            if (fr.fundingTime > endTime) continue;    // beyond requested range
            if (fr.fundingTime <= lastTime) continue;  // already taken in a previous window
            retVal.push_back(fr);
            lastTime = fr.fundingTime;
        }

        cursorSec = batchEndSec; // strictly increases each iteration -> guaranteed termination
    }
    return retVal;
}

std::vector<FundingRate> RESTClient::getCurrentFundingRates() const {
    m_p->throttle();
    const auto response = m_p->checkTransport(m_p->httpSession->get(kFundingRatesPath, nlohmann::json::object(), m_p->authToken));
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

            // The snapshot quotes an 8-HOUR-EQUIVALENT rate while Lighter settles
            // hourly at 1/8 of it — convert so this method returns the same
            // quantity as getFundingRates (settled-per-hour fraction).
            //
            // Proof (2026-07-31, against the venue's own settled cashflows): the
            // /fundings `value` field is the USDC actually settled per 1 token
            // that hour, and value/price == rate%/100 to 4 significant digits on
            // every checked market, while this snapshot read exactly 8.000x that
            // on floor-rate markets (hourly 1.2e-5 vs snapshot 9.6e-5 on
            // ETH/BTC/SOL/TIA). Both consumers of this method (funding-arb and
            // dirty-carry) accumulate it as ONE SETTLED EVENT PER HOUR, so
            // returning the raw quote inflated every accumulated window ~8x
            // against the correctly-seeded history.
            fr.fundingRate /= 8.0;

            rates.push_back(fr);
        }
    }
    return rates;
}

AccountBalance RESTClient::getBalance(const std::int32_t accountIndex, const bool activeOnly) const {
    return m_p->getAccountBalance("index", std::to_string(accountIndex), activeOnly);
}

AccountBalance RESTClient::getBalance(const std::string& l1Address, const bool activeOnly) const {
    return m_p->getAccountBalance("l1_address", l1Address, activeOnly);
}

std::int64_t RESTClient::getNextNonce(const std::int32_t accountIndex, const int apiKeyIndex) const {
    m_p->throttle();
    nlohmann::json query;
    query["account_index"] = accountIndex;
    query["api_key_index"] = apiKeyIndex;

    const auto response = m_p->checkTransport(m_p->httpSession->get(kNextNoncePath, query, m_p->authToken));
    const auto json = m_p->parseEnvelope(response);

    std::int64_t nonce = -1;
    readValue<std::int64_t>(json, "nonce", nonce);

    // -1 fed to the signer means "self-fetch", which would silently decouple the
    // gateway's optimistic counter from the chain — fail loudly instead.
    if (nonce < 0) {
        throw std::runtime_error(fmt::format("Lighter nextNonce: no nonce in response: {}", json.dump()));
    }

    return nonce;
}

SendTxResult RESTClient::sendTx(const std::uint8_t txType, const std::string& txInfo) const {
    m_p->throttle();
    const std::string body = fmt::format("tx_type={}&tx_info={}", static_cast<int>(txType), urlEncode(txInfo));

    // Deliberately NOT parseEnvelope: a venue reject (envelope code != 200) must
    // reach the caller as SendTxResult.code so the gateway can resync its nonce
    // and classify the reject. Throwing is reserved for transport errors and
    // malformed bodies, where the tx outcome is genuinely unknown.
    const auto response = m_p->checkTransport(m_p->httpSession->postForm(kSendTxPath, body, m_p->authToken));
    const auto json = nlohmann::json::parse(response.body());

    SendTxResult result;
    result.fromJson(json);
    return result;
}

void RESTClient::setAuthToken(std::string authToken) const {
    m_p->authToken = std::move(authToken);
}

} // namespace stonky::lighter
