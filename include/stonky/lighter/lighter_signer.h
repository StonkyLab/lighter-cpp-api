/**
Lighter Transaction Signer

Thin C++ wrapper around Lighter's prebuilt native signer (the same Go c-shared
library the official Python/TS SDKs FFI into). Lighter orders are zk-rollup L2
transactions signed with a zk-friendly scheme that is NOT reproducible with
stdlib crypto, so — exactly like the reference SDKs — we delegate signing to the
vendored `third_party/lighter-signer/*.so` and only marshal arguments/results.

The signer holds the API key internally (registered by CreateClient, keyed by
apiKeyIndex + accountIndex); each sign call produces a `txInfo` JSON blob that is
submitted verbatim via RESTClient::sendTx. Nonce is supplied by the caller (the
gateway owns an optimistic nonce counter seeded from /nextNonce).

Licensed under the MIT License <http://opensource.org/licenses/MIT>.
SPDX-License-Identifier: MIT
Copyright (c) 2026 Vitezslav Kot <vitezslav.kot@stonky.cz>, Stonky s.r.o.
*/

#ifndef INCLUDE_STONKY_LIGHTER_SIGNER_H
#define INCLUDE_STONKY_LIGHTER_SIGNER_H

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace stonky::lighter {

/// Order type (matches the signer's Go enum / the Python SDK constants).
enum class LighterOrderType : int {
    Limit = 0,
    Market = 1,
    StopLoss = 2,
    StopLossLimit = 3,
    TakeProfit = 4,
    TakeProfitLimit = 5,
    Twap = 6,
};

/// Time-in-force.
enum class LighterTimeInForce : int {
    ImmediateOrCancel = 0, ///< IOC — use with Market / taker close
    GoodTillTime = 1,
    PostOnly = 2, ///< maker-or-reject — the chase's normal mode
};

/// Sentinels shared with the signer (see the Python SDK).
inline constexpr int LIGHTER_NIL_TRIGGER_PRICE = 0;
inline constexpr std::int64_t LIGHTER_DEFAULT_28_DAY_EXPIRY = -1; ///< limit/post-only default
inline constexpr std::int64_t LIGHTER_DEFAULT_IOC_EXPIRY = 0;     ///< market/IOC

/// A signed transaction ready to submit via RESTClient::sendTx.
struct SignedTx {
    std::uint8_t txType{}; ///< tx type discriminator required by sendTx
    std::string txInfo;    ///< the signed tx JSON payload
    std::string txHash;    ///< venue tx hash (logging / correlation)
};

/// A freshly generated API key pair (register the public key on-chain, keep the
/// private key for the signer).
struct ApiKeyPair {
    std::string privateKey;
    std::string publicKey;
};

/// Generate a new API key pair via the native signer. Pure — no client/account
/// needed; also doubles as a link/load smoke of the vendored .so.
[[nodiscard]] ApiKeyPair generateApiKey();

/**
 * Owns one registered API key (via the native signer's global client table).
 * Construction registers the client; a bad key/url throws. All sign methods are
 * mutex-serialised — the native signer's per-client thread-safety is
 * undocumented and signing is sub-millisecond, so serialising is cheap safety.
 */
class LighterSigner {
    std::mutex m_signMutex;
    int m_apiKeyIndex{};
    std::int64_t m_accountIndex{};
    int m_chainId{};

public:
    /**
     * @param url e.g. "https://mainnet.zklighter.elliot.ai"; the chain id is
     *        derived from it (304 mainnet, 300 testnet) as in the reference SDK.
     * @param apiPrivateKey the API key private key (hex; a leading 0x is trimmed)
     * @param apiKeyIndex the on-chain API key slot registered to the account
     * @param accountIndex the Lighter account index
     * @throws std::runtime_error when the native CreateClient rejects the key
     */
    LighterSigner(const std::string &url, const std::string &apiPrivateKey, int apiKeyIndex, std::int64_t accountIndex);

    /// Verify the registered key matches the account on Lighter. Returns the
    /// venue error string, empty on success.
    [[nodiscard]] std::string checkClient();

    /**
     * Sign a create-order tx. Amounts are venue integers: baseAmount =
     * qty * 10^sizeDecimals, price = px * 10^priceDecimals.
     * @throws std::runtime_error on signer error
     */
    [[nodiscard]] SignedTx signCreateOrder(int marketIndex, std::int64_t clientOrderIndex, std::int64_t baseAmount, std::uint32_t price, bool isAsk, LighterOrderType orderType,
                                           LighterTimeInForce timeInForce, bool reduceOnly, std::int64_t nonce, std::uint32_t triggerPrice = LIGHTER_NIL_TRIGGER_PRICE,
                                           std::int64_t orderExpiry = LIGHTER_DEFAULT_28_DAY_EXPIRY);

    /// Sign a cancel-order tx (by the VENUE order index, not the client id).
    [[nodiscard]] SignedTx signCancelOrder(int marketIndex, std::int64_t orderIndex, std::int64_t nonce);

    /// Sign a cancel-all-orders tx (timeInForce = CANCEL_ALL_TIF_*).
    [[nodiscard]] SignedTx signCancelAllOrders(int timeInForce, std::int64_t timeMs, std::int64_t nonce);

    /// Mint a read-only auth token valid until `deadlineUnix` (seconds). Used to
    /// raise REST rate limits; empty string on error.
    [[nodiscard]] std::string createAuthToken(std::int64_t deadlineUnix);

    [[nodiscard]] int apiKeyIndex() const { return m_apiKeyIndex; }
    [[nodiscard]] std::int64_t accountIndex() const { return m_accountIndex; }
};

} // namespace stonky::lighter

#endif // INCLUDE_STONKY_LIGHTER_SIGNER_H
