/**
Lighter REST Client

Licensed under the MIT License <http://opensource.org/licenses/MIT>.
SPDX-License-Identifier: MIT
Copyright (c) 2026 Vitezslav Kot <vitezslav.kot@stonky.cz>, Stonky s.r.o.
*/

#ifndef INCLUDE_STONKY_LIGHTER_REST_CLIENT_H
#define INCLUDE_STONKY_LIGHTER_REST_CLIENT_H

#include "stonky/lighter/lighter_models.h"
#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace stonky::lighter {

using onCandlesDownloaded = std::function<void(const std::vector<Candle> &)>;

class RESTClient {
    struct P;
    std::unique_ptr<P> m_p{};

public:
    /**
     * Default constructor — unauthenticated Standard tier.
     * Rate: 60 requests/min (one request every 1000 ms).
     * @see https://apidocs.lighter.xyz/docs/rate-limits
     */
    RESTClient();

    /**
     * Authenticated constructor — uses a Lighter read-only auth token to bypass
     * IP-based rate limits and request a higher per-L1-address quota.
     * @param authToken Read-only auth token. Generate via the Lighter Python SDK
     *        (`SignerClient.create_auth_token_with_expiry`) or the createToken
     *        REST endpoint; format: `ro:account_index:scope:expiry_unix:nonce_hex`.
     *        Sent verbatim as the `Authorization` header on every request.
     * @param minRequestInterval Minimum time between successive HTTP requests,
     *        sized to the user's tier. Defaults to 75 ms (Builder tier: 800/min
     *        on /candles). For Plus tier pass 150 ms; for Premium pass 750 ms.
     *        @see https://apidocs.lighter.xyz/docs/account-types
     */
    explicit RESTClient(std::string authToken,
                        std::chrono::milliseconds minRequestInterval = std::chrono::milliseconds(75));

    ~RESTClient();

    /**
     * Download historical candles by symbol (e.g. "BTC"). The symbol→market_id mapping
     * is resolved lazily via /orderBookDetails and cached for the life of the client.
     * @param symbol Market symbol (e.g. "BTC", "ETH")
     * @param interval Candle interval
     * @param from timestamp in ms, must be smaller than "to"
     * @param to timestamp in ms, must be bigger than "from"
     * @param writer Optional callback invoked once per network batch with that batch's
     *        in-range candles (already filtered/deduped, chronologically ascending)
     * @return vector of Candle structures, chronologically ascending, with openTime in
     *         [from, to). A still-forming final candle (openTime + interval > to) is excluded.
     * @throws std::exception when the symbol is unknown or transport fails
     * @see https://apidocs.lighter.xyz/reference/candles
     */
    [[nodiscard]] std::vector<Candle> getHistoricalPrices(const std::string& symbol, CandleInterval interval,
                                                          std::int64_t from, std::int64_t to,
                                                          const onCandlesDownloaded& writer = {}) const;

    /**
     * Download historical candles by Lighter market_id (e.g. 1 = BTC perp). Same range
     * semantics as the symbol overload: returned candles have openTime in [from, to).
     */
    [[nodiscard]] std::vector<Candle> getHistoricalPrices(std::int32_t marketId, CandleInterval interval,
                                                          std::int64_t from, std::int64_t to,
                                                          const onCandlesDownloaded& writer = {}) const;

    /**
     * Download historical funding rates by symbol. Lighter funding period is 1 hour.
     * startTime/endTime are in ms; events are filtered to [startTime, endTime].
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

    /**
     * Get account balance, open positions and asset holdings by account index.
     * @param accountIndex Lighter account index (integer, visible in the UI).
     * @param activeOnly When true (default) the API filters out flat (zero-size) positions.
     * @return AccountBalance with availableBalance, collateral, positions and assets.
     * @throws std::exception when the account is not found or transport fails.
     * @see https://apidocs.lighter.xyz/reference/account-1
     */
    [[nodiscard]] AccountBalance getBalance(std::int32_t accountIndex, bool activeOnly = true) const;

    /**
     * Get account balance by L1 (Ethereum) address.
     * @param l1Address Hex address, e.g. "0xAbCd...".
     */
    [[nodiscard]] AccountBalance getBalance(const std::string& l1Address, bool activeOnly = true) const;

    /**
     * Next transaction nonce for (accountIndex, apiKeyIndex). Seed a local
     * optimistic counter with this and increment per signed tx.
     * @see https://apidocs.lighter.xyz/reference/nextnonce
     */
    [[nodiscard]] std::int64_t getNextNonce(std::int32_t accountIndex, int apiKeyIndex) const;

    /**
     * Submit a signed transaction produced by LighterSigner. The tx is
     * self-authenticating (carries the signature) — no auth token required.
     * @param txType SignedTx::txType   @param txInfo SignedTx::txInfo (JSON)
     * @return SendTxResult; code == 200 means accepted (txHash set)
     * @throws std::exception on transport/envelope error
     * @see https://apidocs.lighter.xyz/reference/sendtx
     */
    [[nodiscard]] SendTxResult sendTx(std::uint8_t txType, const std::string& txInfo) const;
};

} // namespace stonky::lighter

#endif // INCLUDE_STONKY_LIGHTER_REST_CLIENT_H
