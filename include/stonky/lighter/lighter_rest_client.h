/**
Lighter REST Client

Licensed under the MIT License <http://opensource.org/licenses/MIT>.
SPDX-License-Identifier: MIT
Copyright (c) 2026 Vitezslav Kot <vitezslav.kot@stonky.cz>, Stonky s.r.o.
*/

#ifndef INCLUDE_STONKY_LIGHTER_REST_CLIENT_H
#define INCLUDE_STONKY_LIGHTER_REST_CLIENT_H

#include "stonky/lighter/lighter_models.h"
#include <functional>
#include <memory>
#include <string>

namespace stonky::lighter {

using onCandlesDownloaded = std::function<void(const std::vector<Candle> &)>;

class RESTClient {
    struct P;
    std::unique_ptr<P> m_p{};

public:
    RESTClient();

    ~RESTClient();

    /**
     * Download historical candles by symbol (e.g. "BTC"). The symbol→market_id mapping
     * is resolved lazily via /orderBookDetails and cached for the life of the client.
     * @param symbol Market symbol (e.g. "BTC", "ETH")
     * @param interval Candle interval
     * @param from timestamp in ms, must be smaller than "to"
     * @param to timestamp in ms, must be bigger than "from"
     * @param writer Optional callback invoked for each downloaded batch
     * @return vector of Candle structures, chronologically ascending
     * @throws std::exception when the symbol is unknown or transport fails
     * @see https://apidocs.lighter.xyz/reference/candles
     */
    [[nodiscard]] std::vector<Candle> getHistoricalPrices(const std::string& symbol, CandleInterval interval,
                                                          std::int64_t from, std::int64_t to,
                                                          const onCandlesDownloaded& writer = {}) const;

    /**
     * Download historical candles by Lighter market_id (e.g. 0 = BTC perp).
     */
    [[nodiscard]] std::vector<Candle> getHistoricalPrices(std::int32_t marketId, CandleInterval interval,
                                                          std::int64_t from, std::int64_t to,
                                                          const onCandlesDownloaded& writer = {}) const;

    /**
     * Download historical funding rates by symbol. Lighter funding period is 1 hour.
     * @return vector of FundingRate structures, chronologically ascending
     * @throws std::exception
     * @see https://apidocs.lighter.xyz/reference/fundings
     */
    [[nodiscard]] std::vector<FundingRate> getFundingRates(const std::string& symbol, std::int64_t startTime,
                                                            std::int64_t endTime) const;

    /**
     * Download historical funding rates by market_id.
     */
    [[nodiscard]] std::vector<FundingRate> getFundingRates(std::int32_t marketId, std::int64_t startTime,
                                                            std::int64_t endTime) const;

    /**
     * Snapshot of the current funding rate for every market.
     * @see https://apidocs.lighter.xyz/reference/funding-rates
     */
    [[nodiscard]] std::vector<FundingRate> getCurrentFundingRates() const;

    /**
     * Get all perpetual assets (markets).
     * @param includeDelisted If true, non-active markets are included in the result.
     * @see https://apidocs.lighter.xyz/reference/orderbookdetails
     */
    [[nodiscard]] std::vector<PerpAsset> getPerpetualAssets(bool includeDelisted = false) const;
};

} // namespace stonky::lighter

#endif // INCLUDE_STONKY_LIGHTER_REST_CLIENT_H
