/**
Lighter Execution Gateway

Licensed under the MIT License <http://opensource.org/licenses/MIT>.
SPDX-License-Identifier: MIT
Copyright (c) 2026 Vitezslav Kot <vitezslav.kot@stonky.cz>, Stonky s.r.o.
*/

#include <optional>
#include "stonky/lighter/lighter_execution_gateway.h"
#include "stonky/lighter/lighter_rest_client.h"
#include "stonky/lighter/lighter_signer.h"
#include "stonky/lighter/lighter_ws_stream_manager.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace stonky::execution {
using namespace stonky::lighter;

namespace {
void logForwarder(const LogSeverity severity, const std::string &message) {
    switch (severity) {
        case LogSeverity::Info:
            spdlog::info(message);
            break;
        case LogSeverity::Warning:
            spdlog::warn(message);
            break;
        case LogSeverity::Critical:
        case LogSeverity::Error:
            spdlog::error(message);
            break;
        default:
            spdlog::debug(message);
            break;
    }
}

double pow10i(const int n) {
    double r = 1.0;
    for (int i = 0; i < n; ++i) {
        r *= 10.0;
    }
    return r;
}

std::string toLower(std::string s) {
    std::ranges::transform(s, s.begin(), [](const unsigned char c) { return std::tolower(c); });
    return s;
}

std::int64_t nowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

double readStr(const nlohmann::json &json, const char *key) {
    if (const auto it = json.find(key); it != json.end()) {
        if (it->is_string()) {
            try {
                return std::stod(it->get<std::string>());
            } catch (...) {
                return 0.0;
            }
        }
        if (it->is_number()) {
            return it->get<double>();
        }
    }
    return 0.0;
}

/// Map a Lighter order status to (state, rejectKind). Statuses per the API's
/// order-status enum; the canceled-* variants are terminal rejects with a cause.
std::pair<OrderState, RejectKind> classifyStatus(const std::string &status) {
    if (status == "open" || status == "pending" || status == "in-progress") {
        return {OrderState::Accepted, RejectKind::Hard};
    }
    if (status == "filled") {
        return {OrderState::Filled, RejectKind::Hard};
    }
    if (status == "canceled") {
        return {OrderState::Cancelled, RejectKind::Hard}; // our own cancel
    }
    if (status == "canceled-post-only") {
        return {OrderState::Rejected, RejectKind::BenignPostOnlyCross};
    }
    if (status == "canceled-not-enough-liquidity" || status == "canceled-too-much-slippage") {
        return {OrderState::Rejected, RejectKind::Hard};
    }
    if (status == "canceled-reduce-only" || status == "canceled-position-not-allowed" || status == "canceled-margin-not-allowed" || status == "canceled-invalid-balance") {
        return {OrderState::Rejected, RejectKind::Permanent};
    }
    if (status.starts_with("canceled")) {
        return {OrderState::Cancelled, RejectKind::Hard};
    }
    return {OrderState::Accepted, RejectKind::Hard};
}

RejectKind classifyReject(const std::string &reason) {
    const auto r = toLower(reason);
    if (r.find("post") != std::string::npos && r.find("only") != std::string::npos) {
        return RejectKind::BenignPostOnlyCross;
    }
    /// 23000 "Not enough volume quota" — the account's tx allowance (volume-
    /// quota program: +1 tx per $2 filled, one free tx per 15 s) is exhausted.
    /// Retryable after a pause, NOT a dead order path.
    if (r.find("23000") != std::string::npos || r.find("volume quota") != std::string::npos) {
        return RejectKind::Throttled;
    }
    /// 21706 "invalid order base or quote amount" — the venue can never accept
    /// this (symbol, sizing) combination; retries within the cycle are futile.
    if (r.find("21706") != std::string::npos) {
        return RejectKind::Permanent;
    }
    if (r.find("nonce") != std::string::npos) {
        return RejectKind::Hard; // resynced by the caller
    }
    if (r.find("margin") != std::string::npos || r.find("balance") != std::string::npos || r.find("insufficient") != std::string::npos) {
        return RejectKind::Hard;
    }
    if (r.find("min") != std::string::npos && (r.find("base") != std::string::npos || r.find("size") != std::string::npos || r.find("amount") != std::string::npos)) {
        return RejectKind::MinNotional;
    }
    return RejectKind::Hard;
}

bool isOrderGoneReason(const std::string &reason) {
    const auto r = toLower(reason);
    return r.find("not found") != std::string::npos || r.find("not exist") != std::string::npos || r.find("already") != std::string::npos ||
           r.find("inactive") != std::string::npos;
}
} // namespace

struct LighterExecutionGateway::P {
    std::unique_ptr<LighterSigner> signer;
    std::unique_ptr<RESTClient> restClient;
    std::unique_ptr<WSStreamManager> stream;

    std::int64_t accountIndex{};
    int apiKeyIndex{};

    onOrderUpdateEvent orderUpdateCB;
    onFillEvent fillCB;
    onQuoteEvent quoteCB;

    // ── Instrument metadata ─────────────────────────────────────────
    std::mutex specM;
    std::map<std::string, int> symbolToMarketId;
    std::map<int, std::string> marketIdToSymbol;
    std::map<std::string, InstrumentSpec> specCache;
    std::map<std::string, int> sizeDecimals;
    std::map<std::string, int> priceDecimals;

    // ── Quotes ──────────────────────────────────────────────────────
    std::mutex quoteM;
    std::map<std::string, Quote> quoteCache;

    // ── Orders ──────────────────────────────────────────────────────
    struct OrderRec {
        std::string clientOrderId;
        std::string symbol;
        int marketId{-1};
        std::int64_t orderIndex{-1}; ///< venue index (learned from account events; needed to cancel)
        double cumFilledBase{0.0};   ///< dedup for order-channel fill accounting
        double submittedQty{0.0};    ///< plausibility bound for cumulative fills
    };

    std::mutex orderM;
    std::map<std::int64_t, OrderRec> ordersByIndex;     ///< by our clientOrderIndex
    std::map<std::string, std::int64_t> clientIdToIndex; ///< clientOrderId → clientOrderIndex
    /// Seeded from epoch-ms in the ctor: a restart must never reuse indices that
    /// a stray order from the previous run may still carry at the venue.
    std::atomic<std::int64_t> clientOrderCounter{1};

    /// Set once the venue acknowledges the account_all_orders subscription —
    /// start() gates on it so we never trade without the private order feed.
    std::atomic<bool> accountFeedLive{false};

    // ── Nonce / tx serialisation ────────────────────────────────────
    // Lighter nonces are strictly sequential per API key, so every order op is
    // serialised here and the nonce is resynced from REST after any failure.
    std::mutex txM;
    std::int64_t nextNonce{0};
    /// Guards the auth-token re-mint so it is safe to drive from BOTH the write
    /// path (holding txM) and the read path (holding nothing) — see
    /// refreshAuthTokenIfNeeded. Innermost lock: never acquire another under it.
    std::mutex authM;
    std::int64_t authTokenDeadline{0}; ///< unix s; re-mint before it passes

    void eraseOrderRec(const std::int64_t clientOrderIndex, const std::string &clientOrderId) {
        std::lock_guard lk(orderM);
        ordersByIndex.erase(clientOrderIndex);
        clientIdToIndex.erase(clientOrderId);
    }

    /// (Re)load instrument metadata → symbol/marketId + spec caches. Called at
    /// start() and again on an unknown-symbol miss, so markets listed after
    /// startup resolve instead of aborting the leg.
    void loadInstruments() {
        /// The read-path token refresh: refreshInstruments() runs at the top of
        /// every rebalance (before any read that carries the token), so this is
        /// the choke point that keeps a stable-book process from letting its
        /// auth token expire. Cheap and idempotent (no-op until the 1 h margin).
        refreshAuthTokenIfNeeded();

        for (const auto &asset: restClient->getPerpetualAssets(false)) {
            std::lock_guard lk(specM);
            symbolToMarketId[asset.symbol] = asset.marketId;
            marketIdToSymbol[asset.marketId] = asset.symbol;
            sizeDecimals[asset.symbol] = asset.sizeDecimals;
            priceDecimals[asset.symbol] = asset.priceDecimals;

            InstrumentSpec spec;
            spec.symbol = asset.symbol;
            spec.tickSize = 1.0 / pow10i(asset.priceDecimals);
            spec.qtyStep = 1.0 / pow10i(asset.sizeDecimals);
            spec.minQty = asset.minBaseAmount;
            spec.maxQty = 0.0; // no venue cap exposed
            spec.minNotional = asset.minQuoteAmount;
            specCache[asset.symbol] = spec;
        }
    }

    [[nodiscard]] int marketIdFor(const std::string &symbol) {
        std::lock_guard lk(specM);
        const auto it = symbolToMarketId.find(symbol);
        return it != symbolToMarketId.end() ? it->second : -1;
    }

    [[nodiscard]] std::string symbolFor(const int marketId) {
        std::lock_guard lk(specM);
        const auto it = marketIdToSymbol.find(marketId);
        return it != marketIdToSymbol.end() ? it->second : std::string{};
    }

    void resyncNonce() {
        try {
            nextNonce = restClient->getNextNonce(static_cast<std::int32_t>(accountIndex), apiKeyIndex);
        } catch (std::exception &e) {
            spdlog::error("LighterGW nonce resync failed: {}", e.what());
        }
    }

    /// Lighter auth tokens live 8 h max; the bot runs for days. Re-mint (local,
    /// sub-ms, signer-mutex-guarded) with an hour of margin.
    ///
    /// MUST be driven from the READ path, not only writes: a stable book
    /// (hysteresis → no order flow) issues no signed tx for hours, so a
    /// write-only refresh let the token expire and then EVERY read 401'd —
    /// each rebalance starts with refreshInstruments (a read), which died
    /// before any write could re-mint (observed 2026-07-21: the bot stopped
    /// trading for 3 days, "invalid auth: expired token"). Self-locked on authM
    /// so it is safe to call from any path (write path holds txM; read path
    /// holds nothing).
    void refreshAuthTokenIfNeeded() {
        std::lock_guard lk(authM);

        const auto now = nowSeconds();
        if (now < authTokenDeadline - 3600) {
            return;
        }

        try {
            const auto deadline = now + 8 * 3600;
            restClient->setAuthToken(signer->createAuthToken(deadline));
            authTokenDeadline = deadline;
            spdlog::info("LighterGW: auth token refreshed (valid to {})", deadline);
        } catch (std::exception &e) {
            // Keep trading on the stale token rather than aborting the op; the
            // next op retries the mint.
            spdlog::error("LighterGW: auth token refresh failed: {}", e.what());
        }
    }

    /// A non-2xx HTTP response from the REST layer arrives here as an exception
    /// whose text embeds Lighter's own {"code","message"} body ("... msg: {...}").
    /// When that body parses to a venue error code, the reject is DEFINITIVE
    /// (order never accepted, nonce not consumed) — extract it so the caller can
    /// throw a structured GatewayError instead of the ambiguous transport path.
    /// Empty optional = no parseable venue verdict → genuine transport unknown.
    static std::optional<std::pair<int, std::string>> parseVenueReject(const std::string &text) {
        const auto marker = text.find("msg: ");
        if (marker == std::string::npos) {
            return std::nullopt;
        }
        try {
            const auto json = nlohmann::json::parse(text.substr(marker + 5));
            const auto code = json.find("code");
            if (code == json.end() || !code->is_number_integer() || code->get<int>() == 200) {
                return std::nullopt;
            }
            std::string message;
            if (const auto it = json.find("message"); it != json.end() && it->is_string()) {
                message = it->get<std::string>();
            }
            return std::make_pair(code->get<int>(), std::move(message));
        } catch (...) {
            return std::nullopt;
        }
    }

    /// Submit one signed create/cancel tx under the tx lock; resyncs the nonce on
    /// EVERY failure and classifies venue rejects into GatewayError — including
    /// rejects delivered as HTTP 400/429 (their body is a definitive venue
    /// verdict; treating them as transport-ambiguous made the consumer run its
    /// orphan/safety-cancel path, and each phantom cancel burned a volume-quota
    /// free-tx slot — the observed 23000 starvation spiral). A true transport
    /// error (no parseable venue body) is rethrown plain — the tx outcome is
    /// unknown, which is exactly the consumer's orphan-handling path.
    void sendSigned(const SignedTx &tx, const char *what) {
        refreshAuthTokenIfNeeded();

        SendTxResult result;
        try {
            result = restClient->sendTx(tx.txType, tx.txInfo);
        } catch (std::exception &e) {
            // Either way the nonce was not (definitive reject) or may have been
            // (ambiguous transport) consumed — resync so the NEXT op cannot
            // sign with a stale nonce.
            resyncNonce();

            if (const auto reject = parseVenueReject(e.what())) {
                const auto reason = fmt::format("{} {}", reject->first, reject->second);
                throw GatewayError(classifyReject(reason), fmt::format("{}: code {} {}", what, reject->first, reject->second));
            }

            throw;
        }

        if (result.code != 200) {
            resyncNonce();
            throw GatewayError(classifyReject(result.message), fmt::format("{}: code {} {}", what, result.code, result.message));
        }

        ++nextNonce;
    }

    /// Tolerant int read: Lighter stringifies i64 fields, so accept both.
    static std::int64_t readInt(const nlohmann::json &json, const char *key, const std::int64_t fallback = -1) {
        if (const auto it = json.find(key); it != json.end()) {
            if (it->is_number_integer()) {
                return it->get<std::int64_t>();
            }
            if (it->is_string()) {
                try {
                    return std::stoll(it->get<std::string>());
                } catch (...) {
                }
            }
        }
        return fallback;
    }

    // ── Account event parsing (order-driven fills) ──────────────────
    void onAccountMessage(const nlohmann::json &msg) {
        std::string type;
        if (const auto it = msg.find("type"); it != msg.end() && it->is_string()) {
            type = it->get<std::string>();
        }

        // start() gates on this: the order feed is live once the venue acks the
        // account_all_orders subscription.
        if (type.starts_with("subscribed/account_all_orders") || type.starts_with("update/account_all_orders")) {
            accountFeedLive = true;
        } else {
            return; // account_all (positions/balances) — no order content
        }

        // Docs: account_all_orders delivers {"orders": {"<market_index>": [Order]}}.
        // Accept the array shape too — this is exactly the guessed-schema class of
        // risk, so any unparseable shape must be LOUD, never silently dropped.
        std::vector<nlohmann::json> orders;
        if (const auto ordersIt = msg.find("orders"); ordersIt != msg.end()) {
            if (ordersIt->is_array()) {
                orders.assign(ordersIt->begin(), ordersIt->end());
            } else if (ordersIt->is_object()) {
                for (const auto &perMarket: *ordersIt) {
                    if (perMarket.is_array()) {
                        orders.insert(orders.end(), perMarket.begin(), perMarket.end());
                    } else if (perMarket.is_object()) {
                        orders.push_back(perMarket);
                    }
                }
            }
        }

        if (orders.empty()) {
            static std::atomic<int> emptyWarns{0};
            if (msg.find("orders") == msg.end() && emptyWarns.fetch_add(1) < 3) {
                spdlog::warn("LighterGW: {} message without parseable orders — schema mismatch? raw: {}", type, msg.dump());
            }
            return;
        }

        for (const auto &order: orders) {
            const std::int64_t clientOrderIndex = readInt(order, "client_order_index");
            if (clientOrderIndex < 0) {
                spdlog::warn("LighterGW: order event without client_order_index: {}", order.dump());
                continue;
            }

            std::string clientOrderId, symbol;
            double newFill = 0.0;
            double price = readStr(order, "price");

            {
                std::lock_guard lk(orderM);
                const auto recIt = ordersByIndex.find(clientOrderIndex);
                if (recIt == ordersByIndex.end()) {
                    // Possibly a stray order from a previous run — visible, not fatal.
                    spdlog::debug("LighterGW: event for unknown client_order_index {}", clientOrderIndex);
                    continue;
                }
                OrderRec &rec = recIt->second;

                // Learn the venue index once; a DIFFERENT index for the same
                // client_order_index means a cross-run collision — never rebind.
                if (const auto orderIndex = readInt(order, "order_index"); orderIndex >= 0) {
                    if (rec.orderIndex < 0) {
                        rec.orderIndex = orderIndex;
                    } else if (rec.orderIndex != orderIndex) {
                        spdlog::warn("LighterGW: client_order_index {} collision (venue {} vs {}) — event ignored", clientOrderIndex, rec.orderIndex, orderIndex);
                        continue;
                    }
                }

                clientOrderId = rec.clientOrderId;
                symbol = rec.symbol;

                double cumFilled = readStr(order, "filled_base_amount");
                // Plausibility clamp: a fill beyond the submitted qty means a
                // units/schema mismatch, not a real fill — never credit it.
                if (rec.submittedQty > 0.0 && cumFilled > rec.submittedQty * 1.001) {
                    spdlog::error("LighterGW: fill {} exceeds submitted qty {} for {} — clamped (units mismatch?)", cumFilled, rec.submittedQty, clientOrderId);
                    cumFilled = rec.submittedQty;
                }
                if (cumFilled > rec.cumFilledBase) {
                    newFill = cumFilled - rec.cumFilledBase;
                    rec.cumFilledBase = cumFilled;
                }
            }

            std::string status;
            if (const auto it = order.find("status"); it != order.end() && it->is_string()) {
                status = it->get<std::string>();
            }

            // Market dust-closes echo their submitted price (0) — report fills at
            // the cached top-of-book mid instead of $0 notional.
            if (price <= 0.0) {
                std::lock_guard lk(quoteM);
                if (const auto it = quoteCache.find(symbol); it != quoteCache.end() && it->second.sane()) {
                    price = (it->second.bid + it->second.ask) / 2.0;
                }
            }

            // Emit the fill BEFORE the state update so the core credits qty
            // before a terminal state clears the active order (overfill-safe).
            if (fillCB && newFill > 0.0) {
                FillEvent fill;
                fill.clientOrderId = clientOrderId;
                fill.symbol = symbol;
                fill.fillId = fmt::format("{}-{}", clientOrderIndex, cumFilledKey(order));
                fill.qty = newFill;
                fill.price = price;
                fill.isMaker = true;
                fillCB(fill);
            }

            const auto [state, rejectKind] = classifyStatus(status);

            if (orderUpdateCB) {
                OrderUpdate update;
                update.clientOrderId = clientOrderId;
                update.symbol = symbol;
                update.state = state;
                update.price = price;
                update.cumFilledQty = readStr(order, "filled_base_amount");
                update.rejectKind = rejectKind;
                update.reason = status;
                orderUpdateCB(update);
            }

            // Terminal → the record is dead; prune so the maps cannot grow
            // unbounded over a weeks-long run (a re-delivered terminal event
            // then lands in the logged unknown-index branch above).
            if (state == OrderState::Filled || state == OrderState::Cancelled || state == OrderState::Rejected) {
                eraseOrderRec(clientOrderIndex, clientOrderId);
            }

            spdlog::debug("LighterGW order: {} idx={} status={} filled={} px={}", symbol, clientOrderIndex, status, readStr(order, "filled_base_amount"), price);
        }
    }

    /// A per-increment key so re-delivered messages dedup at the core.
    static std::string cumFilledKey(const nlohmann::json &order) {
        if (const auto it = order.find("filled_base_amount"); it != order.end()) {
            return it->is_string() ? it->get<std::string>() : it->dump();
        }
        return "0";
    }
};

LighterExecutionGateway::LighterExecutionGateway(const std::string &url, const std::string &apiPrivateKey, const int apiKeyIndex, const std::int64_t accountIndex) :
    m_p(std::make_unique<P>()) {
    m_p->accountIndex = accountIndex;
    m_p->apiKeyIndex = apiKeyIndex;
    m_p->signer = std::make_unique<LighterSigner>(url, apiPrivateKey, apiKeyIndex, accountIndex);

    // Epoch-ms seed: client_order_index is the venue-side correlation key, so a
    // restart must never collide with a stray order from a previous run.
    m_p->clientOrderCounter = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();

    // A read-only auth token lifts the REST rate limit off the shared IP tier
    // (60/min) onto the account's per-L1 quota — the chase does frequent
    // sendTx/cancel. Minting is local signing with the same key that signs
    // orders, so a failure here means order signing cannot work either: fail
    // fast instead of silently degrading to a tier the chase would overrun.
    const auto deadline = nowSeconds() + 8 * 3600;
    const auto authToken = m_p->signer->createAuthToken(deadline);
    if (authToken.empty()) {
        throw std::runtime_error("Lighter gateway: auth token mint failed — check the API key");
    }
    m_p->authTokenDeadline = deadline;

    m_p->restClient = std::make_unique<RESTClient>(authToken, std::chrono::milliseconds(100));
    m_p->stream = std::make_unique<WSStreamManager>();
    m_p->stream->setLoggerCallback(&logForwarder);
}

LighterExecutionGateway::~LighterExecutionGateway() {
    // Join the WS io thread FIRST: default member order would destroy the maps,
    // mutexes and callbacks while the io thread is still delivering into them.
    if (m_p && m_p->stream) {
        m_p->stream.reset();
    }
}

std::string LighterExecutionGateway::name() const { return "Lighter"; }

void LighterExecutionGateway::start() {
    m_p->loadInstruments();

    // Seed the sequential nonce.
    m_p->nextNonce = m_p->restClient->getNextNonce(static_cast<std::int32_t>(m_p->accountIndex), m_p->apiKeyIndex);

    // Wire the stream callbacks.
    m_p->stream->setOrderBookCallback([this](const int marketId, const double bestBid, const double bestAsk) {
        const auto symbol = m_p->symbolFor(marketId);
        if (symbol.empty()) {
            return;
        }

        Quote quote;
        quote.bid = bestBid;
        quote.ask = bestAsk;
        quote.receivedAt = std::chrono::steady_clock::now();

        {
            std::lock_guard lk(m_p->quoteM);
            m_p->quoteCache[symbol] = quote;
        }

        if (m_p->quoteCB) {
            m_p->quoteCB(symbol, quote);
        }
    });

    // Exercise the credentials against the venue NOW — without this, a wrong
    // key/account pairing surfaces only at the first live order submit.
    if (const auto err = m_p->signer->checkClient(); !err.empty()) {
        spdlog::warn("LighterGW: credential check failed ({}) — order submits will likely be rejected", err);
    }

    m_p->stream->setAccountCallback([this](const nlohmann::json &msg) { m_p->onAccountMessage(msg); });

    // Order events live on account_all_orders (auth channel). The token is
    // minted fresh at every (re)subscribe so reconnect replays never carry an
    // expired one.
    m_p->stream->subscribeAccountOrders(static_cast<int>(m_p->accountIndex), [this](const std::string &) -> std::string {
        try {
            return m_p->signer->createAuthToken(nowSeconds() + 8 * 3600);
        } catch (std::exception &e) {
            spdlog::error("LighterGW: WS auth token mint failed: {}", e.what());
            return {};
        }
    });
    m_p->stream->start();

    // Block until the venue ACKS the order-feed subscription — a mere transport
    // greeting is not "feed live", and trading without the private order feed
    // silently stacks orders.
    for (int i = 0; i < 100 && !m_p->accountFeedLive.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (!m_p->accountFeedLive.load()) {
        throw std::runtime_error("Lighter gateway: account_all_orders subscription not acknowledged within 10 s");
    }

    spdlog::info("LighterExecutionGateway: ready ({} instruments)", m_p->specCache.size());
}

InstrumentSpec LighterExecutionGateway::instrumentSpec(const std::string &symbol) {
    {
        std::lock_guard lk(m_p->specM);
        if (const auto it = m_p->specCache.find(symbol); it != m_p->specCache.end()) {
            return it->second;
        }
    }

    // Miss → the market may have listed after start(); refresh once and retry.
    m_p->loadInstruments();

    std::lock_guard lk(m_p->specM);
    const auto it = m_p->specCache.find(symbol);
    if (it == m_p->specCache.end()) {
        throw std::runtime_error(fmt::format("Lighter: unknown instrument {}", symbol));
    }
    return it->second;
}

void LighterExecutionGateway::refreshInstruments() {
    /// Full reload — loadInstruments overwrites specs/decimals in place.
    m_p->loadInstruments();
}

void LighterExecutionGateway::subscribeQuotes(const std::string &symbol) {
    const int marketId = m_p->marketIdFor(symbol);
    if (marketId < 0) {
        throw std::runtime_error(fmt::format("Lighter: unknown instrument {}", symbol));
    }
    m_p->stream->subscribeOrderBook(marketId);
}

void LighterExecutionGateway::unsubscribeQuotes(const std::string &symbol) {
    const int marketId = m_p->marketIdFor(symbol);
    if (marketId >= 0) {
        m_p->stream->unsubscribeOrderBook(marketId);
    }
}

std::optional<Quote> LighterExecutionGateway::lastQuote(const std::string &symbol) {
    std::lock_guard lk(m_p->quoteM);
    const auto it = m_p->quoteCache.find(symbol);
    if (it == m_p->quoteCache.end()) {
        return std::nullopt;
    }
    return it->second;
}

void LighterExecutionGateway::setOrderUpdateCallback(const onOrderUpdateEvent &cb) { m_p->orderUpdateCB = cb; }

void LighterExecutionGateway::setFillCallback(const onFillEvent &cb) { m_p->fillCB = cb; }

void LighterExecutionGateway::setQuoteCallback(const onQuoteEvent &cb) { m_p->quoteCB = cb; }

void LighterExecutionGateway::submitPostOnlyLimit(const std::string &clientOrderId, const std::string &symbol, const OrderSide side, const double qty, const double price,
                                                  const bool reduceOnly) {
    int marketId, sizeDec, priceDec;
    {
        std::lock_guard lk(m_p->specM);
        const auto midIt = m_p->symbolToMarketId.find(symbol);
        if (midIt == m_p->symbolToMarketId.end()) {
            throw GatewayError(RejectKind::Permanent, fmt::format("Lighter: unknown instrument {}", symbol));
        }
        marketId = midIt->second;
        sizeDec = m_p->sizeDecimals[symbol];
        priceDec = m_p->priceDecimals[symbol];
    }

    const auto baseAmount = std::llround(qty * pow10i(sizeDec));
    if (baseAmount <= 0) {
        throw GatewayError(RejectKind::MinNotional, fmt::format("Lighter: qty {} rounds to 0 base units", qty));
    }

    // The signer's price field is uint32 — a silent wrap would submit a validly
    // signed order at a wildly wrong limit.
    const auto priceScaled64 = std::llround(price * pow10i(priceDec));
    if (priceScaled64 <= 0 || priceScaled64 > std::numeric_limits<std::uint32_t>::max()) {
        throw GatewayError(RejectKind::Hard, fmt::format("Lighter: price {} scales to {} — outside the venue's uint32 domain", price, priceScaled64));
    }
    const auto priceScaled = static_cast<std::uint32_t>(priceScaled64);

    const std::int64_t clientOrderIndex = m_p->clientOrderCounter.fetch_add(1);
    {
        std::lock_guard lk(m_p->orderM);
        m_p->ordersByIndex[clientOrderIndex] = P::OrderRec{clientOrderId, symbol, marketId, -1, 0.0, qty};
        m_p->clientIdToIndex[clientOrderId] = clientOrderIndex;
    }

    std::lock_guard tx(m_p->txM);
    SignedTx signed_;
    try {
        signed_ = m_p->signer->signCreateOrder(marketId, clientOrderIndex, baseAmount, priceScaled, side == OrderSide::Sell, LighterOrderType::Limit,
                                               LighterTimeInForce::PostOnly, reduceOnly, m_p->nextNonce);
    } catch (std::exception &e) {
        // Sign-time failure: nothing reached the venue, the nonce is untouched.
        m_p->eraseOrderRec(clientOrderIndex, clientOrderId);
        throw GatewayError(RejectKind::Hard, fmt::format("Lighter sign failed: {}", e.what()));
    }

    try {
        m_p->sendSigned(signed_, "submitPostOnlyLimit");
    } catch (GatewayError &) {
        // Definitive venue reject — the order is not resting, drop the record.
        // A plain (transport) exception keeps it: the tx may have landed and the
        // record is needed to attribute the resulting WS events.
        m_p->eraseOrderRec(clientOrderIndex, clientOrderId);
        throw;
    }

    spdlog::debug("LighterGW submit ok: {} {} base={} px={} idx={}", symbol, side == OrderSide::Sell ? "Sell" : "Buy", baseAmount, priceScaled, clientOrderIndex);
}

bool LighterExecutionGateway::supportsAmend() const { return false; }

void LighterExecutionGateway::amendPrice(const std::string &, const std::string &, double) {
    throw GatewayError(RejectKind::Hard, "Lighter: amend not supported (cancel + resubmit)");
}

bool LighterExecutionGateway::cancel(const std::string &clientOrderId, const std::string &symbol) {
    int marketId;
    std::int64_t orderIndex;
    {
        std::lock_guard lk(m_p->orderM);
        const auto idxIt = m_p->clientIdToIndex.find(clientOrderId);
        if (idxIt == m_p->clientIdToIndex.end()) {
            return false; // no record → terminal event already pruned it
        }
        const auto recIt = m_p->ordersByIndex.find(idxIt->second);
        if (recIt == m_p->ordersByIndex.end()) {
            return false;
        }
        if (recIt->second.orderIndex < 0) {
            // Submitted but the venue order index has not arrived yet — the
            // order is most likely RESTING. Returning false here would tell the
            // core "order left the book, stop waiting" and it would resubmit on
            // top of a live order. Throw instead: the core keeps the handle and
            // retries the cancel once the account feed delivers the index.
            throw GatewayError(RejectKind::Hard, fmt::format("Lighter: order {} not yet acknowledged — cancel retried next tick", clientOrderId));
        }
        marketId = recIt->second.marketId;
        orderIndex = recIt->second.orderIndex;
    }

    std::lock_guard tx(m_p->txM);
    SignedTx signed_;
    try {
        signed_ = m_p->signer->signCancelOrder(marketId, orderIndex, m_p->nextNonce);
    } catch (std::exception &e) {
        throw GatewayError(RejectKind::Hard, fmt::format("Lighter sign failed: {}", e.what()));
    }

    try {
        m_p->sendSigned(signed_, "cancel");
        return true;
    } catch (GatewayError &e) {
        if (isOrderGoneReason(e.what())) {
            return false; // venue-confirmed gone — per contract, do not wait
        }
        throw;
    } catch (std::exception &e) {
        // Transport-ambiguous cancel: never report "gone" — rethrow classified
        // so the core keeps the handle and retries.
        throw GatewayError(RejectKind::Hard, fmt::format("Lighter cancel transport failure: {}", e.what()));
    }
}

void LighterExecutionGateway::submitReduceOnlyMarket(const std::string &clientOrderId, const std::string &symbol, const OrderSide side, const double qty) {
    int marketId, sizeDec, priceDec;
    {
        std::lock_guard lk(m_p->specM);
        const auto midIt = m_p->symbolToMarketId.find(symbol);
        if (midIt == m_p->symbolToMarketId.end()) {
            throw GatewayError(RejectKind::Permanent, fmt::format("Lighter: unknown instrument {}", symbol));
        }
        marketId = midIt->second;
        sizeDec = m_p->sizeDecimals[symbol];
        priceDec = m_p->priceDecimals[symbol];
    }

    const auto baseAmount = std::llround(qty * pow10i(sizeDec));
    if (baseAmount <= 0) {
        throw GatewayError(RejectKind::MinNotional, fmt::format("Lighter: qty {} rounds to 0 base units", qty));
    }

    // A Lighter "market" order's price is the WORST-ACCEPTABLE execution bound
    // (avg_execution_price in the reference SDK) and the signer rejects 0
    // (MinOrderPrice=1) — so derive the bound from the live top-of-book with a
    // 5% slippage allowance. Requires a sane cached quote.
    double bound;
    {
        std::lock_guard lk(m_p->quoteM);
        const auto it = m_p->quoteCache.find(symbol);
        if (it == m_p->quoteCache.end() || !it->second.sane()) {
            throw GatewayError(RejectKind::Hard, fmt::format("Lighter: no sane quote for {} — cannot bound the market close", symbol));
        }
        bound = side == OrderSide::Buy ? it->second.ask * 1.05 : it->second.bid * 0.95;
    }

    const auto boundScaled64 = std::llround(bound * pow10i(priceDec));
    const auto boundScaled = static_cast<std::uint32_t>(std::clamp<std::int64_t>(boundScaled64, 1, std::numeric_limits<std::uint32_t>::max()));

    const std::int64_t clientOrderIndex = m_p->clientOrderCounter.fetch_add(1);
    {
        std::lock_guard lk(m_p->orderM);
        m_p->ordersByIndex[clientOrderIndex] = P::OrderRec{clientOrderId, symbol, marketId, -1, 0.0, qty};
        m_p->clientIdToIndex[clientOrderId] = clientOrderIndex;
    }

    std::lock_guard tx(m_p->txM);
    SignedTx signed_;
    try {
        signed_ = m_p->signer->signCreateOrder(marketId, clientOrderIndex, baseAmount, boundScaled, side == OrderSide::Sell, LighterOrderType::Market,
                                               LighterTimeInForce::ImmediateOrCancel, true, m_p->nextNonce, LIGHTER_NIL_TRIGGER_PRICE, LIGHTER_DEFAULT_IOC_EXPIRY);
    } catch (std::exception &e) {
        m_p->eraseOrderRec(clientOrderIndex, clientOrderId);
        throw GatewayError(RejectKind::Hard, fmt::format("Lighter sign failed: {}", e.what()));
    }

    try {
        m_p->sendSigned(signed_, "submitReduceOnlyMarket");
    } catch (GatewayError &) {
        m_p->eraseOrderRec(clientOrderIndex, clientOrderId);
        throw;
    }
}

} // namespace stonky::execution
