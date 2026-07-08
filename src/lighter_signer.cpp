/**
Lighter Transaction Signer

Licensed under the MIT License <http://opensource.org/licenses/MIT>.
SPDX-License-Identifier: MIT
Copyright (c) 2026 Vitezslav Kot <vitezslav.kot@stonky.cz>, Stonky s.r.o.
*/

#include "stonky/lighter/lighter_signer.h"
#include <fmt/format.h>
#include <stdexcept>

#ifdef _WIN32
// ── Windows: native signer unavailable ──────────────────────────────────
// The signer is a Linux-only Go c-shared library (no Windows build — see
// CMakeLists.txt, where it is omitted on MSVC), so these entry points are
// throwing stubs. The data downloader never signs orders; anything that does
// fails fast and loud instead of silently mis-linking.
namespace stonky::lighter {
namespace {
[[noreturn]] void signerUnavailable(const char *what) {
    throw std::runtime_error(fmt::format("Lighter signer '{}' is unavailable on Windows (native Go signer is Linux-only)", what));
}
} // namespace

ApiKeyPair generateApiKey() { signerUnavailable("generateApiKey"); }

LighterSigner::LighterSigner(const std::string & /*url*/, const std::string & /*apiPrivateKey*/, const int apiKeyIndex, const std::int64_t accountIndex) :
    m_apiKeyIndex(apiKeyIndex), m_accountIndex(accountIndex), m_chainId(0) {
    signerUnavailable("LighterSigner");
}

std::string LighterSigner::checkClient() { signerUnavailable("checkClient"); }

SignedTx LighterSigner::signCreateOrder(int /*marketIndex*/, std::int64_t /*clientOrderIndex*/, std::int64_t /*baseAmount*/, std::uint32_t /*price*/, bool /*isAsk*/, LighterOrderType /*orderType*/,
                                        LighterTimeInForce /*timeInForce*/, bool /*reduceOnly*/, std::int64_t /*nonce*/, std::uint32_t /*triggerPrice*/, std::int64_t /*orderExpiry*/) {
    signerUnavailable("signCreateOrder");
}

SignedTx LighterSigner::signCancelOrder(int /*marketIndex*/, std::int64_t /*orderIndex*/, std::int64_t /*nonce*/) { signerUnavailable("signCancelOrder"); }

SignedTx LighterSigner::signCancelAllOrders(int /*timeInForce*/, std::int64_t /*timeMs*/, std::int64_t /*nonce*/) { signerUnavailable("signCancelAllOrders"); }

std::string LighterSigner::createAuthToken(std::int64_t /*deadlineUnix*/) { signerUnavailable("createAuthToken"); }

} // namespace stonky::lighter

#else // !_WIN32

// ── Native signer C ABI ─────────────────────────────────────────────────
// Mirrors third_party/lighter-signer/lighter-signer.h (the cgo-generated
// header). Declared locally so the Go prologue / Go types never leak into
// consumers. Struct layouts must match the vendored header exactly — they are
// returned BY VALUE across the C ABI.
extern "C" {
struct StrOrErr {
    char *str;
    char *err;
};

struct SignedTxResponse {
    std::uint8_t txType;
    char *txInfo;
    char *txHash;
    char *messageToSign;
    char *err;
};

struct ApiKeyResponse {
    char *privateKey;
    char *publicKey;
    char *err;
};

ApiKeyResponse GenerateAPIKey();
char *CreateClient(char *cUrl, char *cPrivateKey, int cChainId, int cApiKeyIndex, long long cAccountIndex);
char *CheckClient(int cApiKeyIndex, long long cAccountIndex);
SignedTxResponse SignCreateOrder(int cMarketIndex, long long cClientOrderIndex, long long cBaseAmount, int cPrice, int cIsAsk, int cOrderType, int cTimeInForce, int cReduceOnly,
                                 int cTriggerPrice, long long cOrderExpiry, long long cIntegratorAccountIndex, int cIntegratorTakerFee, int cIntegratorMakerFee, std::uint8_t cSkipNonce,
                                 long long cNonce, int cApiKeyIndex, long long cAccountIndex);
SignedTxResponse SignCancelOrder(int cMarketIndex, long long cOrderIndex, std::uint8_t cSkipNonce, long long cNonce, int cApiKeyIndex, long long cAccountIndex);
SignedTxResponse SignCancelAllOrders(int cTimeInForce, long long cTime, std::uint8_t cSkipNonce, long long cNonce, int cApiKeyIndex, long long cAccountIndex);
StrOrErr CreateAuthToken(long long cDeadline, int cApiKeyIndex, long long cAccountIndex);
void Free(void *ptr);
}

namespace stonky::lighter {
namespace {
constexpr std::uint8_t SKIP_NONCE_OFF = 0;

/// Copy a Go-allocated C string into std::string and Free it (Go owns the
/// buffer; the reference SDK's decode_and_free does the same). Safe on null.
std::string takeString(char *ptr) {
    if (ptr == nullptr) {
        return {};
    }

    std::string out(ptr);
    Free(ptr);
    return out;
}

/// Marshal a SignedTxResponse: free every returned buffer, throw on error,
/// otherwise return the C++ value.
SignedTx takeSigned(const SignedTxResponse &resp, const char *what) {
    const std::uint8_t txType = resp.txType;
    std::string txInfo = takeString(resp.txInfo);
    std::string txHash = takeString(resp.txHash);
    takeString(resp.messageToSign); // unused (only needed for L1-signed tx types)
    const std::string err = takeString(resp.err);

    if (!err.empty()) {
        throw std::runtime_error(fmt::format("Lighter {}: {}", what, err));
    }

    return SignedTx{txType, std::move(txInfo), std::move(txHash)};
}

/// Lighter's chain id is derived from the host, matching the reference SDK.
int chainIdForUrl(const std::string &url) {
    return url.find("mainnet") != std::string::npos || url.find("api") != std::string::npos ? 304 : 300;
}

/// The signer takes price/triggerPrice as C `int` but treats them as uint32 on
/// the Go side; preserve the bit pattern for values above INT_MAX.
int asSignedBits(const std::uint32_t v) {
    return static_cast<int>(v);
}
} // namespace

ApiKeyPair generateApiKey() {
    const auto resp = GenerateAPIKey();
    std::string priv = takeString(resp.privateKey);
    std::string pub = takeString(resp.publicKey);

    if (const std::string err = takeString(resp.err); !err.empty()) {
        throw std::runtime_error(fmt::format("Lighter GenerateAPIKey: {}", err));
    }

    return ApiKeyPair{std::move(priv), std::move(pub)};
}

LighterSigner::LighterSigner(const std::string &url, const std::string &apiPrivateKey, const int apiKeyIndex, const std::int64_t accountIndex) :
    m_apiKeyIndex(apiKeyIndex), m_accountIndex(accountIndex), m_chainId(chainIdForUrl(url)) {
    std::string key = apiPrivateKey;

    if (key.starts_with("0x")) {
        key = key.substr(2);
    }

    // CreateClient copies the strings; a mutable buffer is required by the C ABI.
    std::string mutableUrl = url;
    char *err = CreateClient(mutableUrl.data(), key.data(), m_chainId, m_apiKeyIndex, m_accountIndex);

    if (const std::string errStr = takeString(err); !errStr.empty()) {
        throw std::runtime_error(fmt::format("Lighter CreateClient: {}", errStr));
    }
}

std::string LighterSigner::checkClient() {
    std::lock_guard lk(m_signMutex);
    return takeString(CheckClient(m_apiKeyIndex, m_accountIndex));
}

SignedTx LighterSigner::signCreateOrder(const int marketIndex, const std::int64_t clientOrderIndex, const std::int64_t baseAmount, const std::uint32_t price, const bool isAsk,
                                        const LighterOrderType orderType, const LighterTimeInForce timeInForce, const bool reduceOnly, const std::int64_t nonce,
                                        const std::uint32_t triggerPrice, const std::int64_t orderExpiry) {
    std::lock_guard lk(m_signMutex);
    const auto resp = SignCreateOrder(marketIndex, clientOrderIndex, baseAmount, asSignedBits(price), isAsk ? 1 : 0, static_cast<int>(orderType), static_cast<int>(timeInForce),
                                      reduceOnly ? 1 : 0, asSignedBits(triggerPrice), orderExpiry, /*integratorAccountIndex*/ 0, /*integratorTakerFee*/ 0, /*integratorMakerFee*/ 0,
                                      SKIP_NONCE_OFF, nonce, m_apiKeyIndex, m_accountIndex);
    return takeSigned(resp, "SignCreateOrder");
}

SignedTx LighterSigner::signCancelOrder(const int marketIndex, const std::int64_t orderIndex, const std::int64_t nonce) {
    std::lock_guard lk(m_signMutex);
    return takeSigned(SignCancelOrder(marketIndex, orderIndex, SKIP_NONCE_OFF, nonce, m_apiKeyIndex, m_accountIndex), "SignCancelOrder");
}

SignedTx LighterSigner::signCancelAllOrders(const int timeInForce, const std::int64_t timeMs, const std::int64_t nonce) {
    std::lock_guard lk(m_signMutex);
    return takeSigned(SignCancelAllOrders(timeInForce, timeMs, SKIP_NONCE_OFF, nonce, m_apiKeyIndex, m_accountIndex), "SignCancelAllOrders");
}

std::string LighterSigner::createAuthToken(const std::int64_t deadlineUnix) {
    std::lock_guard lk(m_signMutex);
    const auto resp = CreateAuthToken(deadlineUnix, m_apiKeyIndex, m_accountIndex);
    std::string token = takeString(resp.str);

    if (const std::string err = takeString(resp.err); !err.empty()) {
        throw std::runtime_error(fmt::format("Lighter CreateAuthToken: {}", err));
    }

    return token;
}

} // namespace stonky::lighter

#endif // _WIN32
