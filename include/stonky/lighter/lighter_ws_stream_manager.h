/**
Lighter WebSocket Stream Manager

Licensed under the MIT License <http://opensource.org/licenses/MIT>.
SPDX-License-Identifier: MIT
Copyright (c) 2026 Vitezslav Kot <vitezslav.kot@stonky.cz>, Stonky s.r.o.
*/

#ifndef INCLUDE_STONKY_LIGHTER_WS_STREAM_MANAGER_H
#define INCLUDE_STONKY_LIGHTER_WS_STREAM_MANAGER_H

#include "stonky/lighter/lighter_ws_session.h"
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>

namespace stonky::lighter {

/// Top-of-book update for a market (human-decimal prices). bestBid/bestAsk are
/// 0 when that side is empty.
using onOrderBookUpdate = std::function<void(int marketId, double bestBid, double bestAsk)>;

/// Raw account_all message (subscribed/update). The gateway extracts order and
/// trade events from it.
using onAccountUpdate = std::function<void(const nlohmann::json &)>;

/**
 * Owns the io context + thread + WebSocketSession and turns Lighter's stream
 * into typed callbacks. Maintains each subscribed market's order book from the
 * snapshot + deltas and emits the best bid/ask on every change. Account messages
 * are passed through raw. Callbacks run on the io thread — keep them fast.
 */
class WSStreamManager {
    struct P;
    std::unique_ptr<P> m_p;

public:
    WSStreamManager();

    ~WSStreamManager();

    void setLoggerCallback(const onLogMessage &cb) const;

    void setOrderBookCallback(const onOrderBookUpdate &cb) const;

    void setAccountCallback(const onAccountUpdate &cb) const;

    /// Override the endpoint (defaults to wss://mainnet.zklighter.elliot.ai/stream).
    void setEndpoint(const std::string &host, const std::string &port, const std::string &path) const;

    /// Connect and start the io thread. Idempotent.
    void start() const;

    void subscribeOrderBook(int marketId) const;

    void unsubscribeOrderBook(int marketId) const;

    /// Subscribe the account_all channel — positions/balances (NO orders).
    void subscribeAccount(int accountIndex) const;

    /**
     * Subscribe the account_all_orders channel — the ORDER event stream. Requires
     * an auth token in the subscribe frame; tokenProvider is invoked at every
     * (re)subscribe so reconnect replays always carry a fresh token.
     */
    void subscribeAccountOrders(int accountIndex, const onAuthTokenProvider &tokenProvider) const;

    [[nodiscard]] bool isConnected() const;
};
} // namespace stonky::lighter

#endif // INCLUDE_STONKY_LIGHTER_WS_STREAM_MANAGER_H
