/**
Lighter WebSocket Session

Licensed under the MIT License <http://opensource.org/licenses/MIT>.
SPDX-License-Identifier: MIT
Copyright (c) 2026 Vitezslav Kot <vitezslav.kot@stonky.cz>, Stonky s.r.o.
*/

#ifndef INCLUDE_STONKY_LIGHTER_WS_SESSION_H
#define INCLUDE_STONKY_LIGHTER_WS_SESSION_H

#include "stonky/utils/log_utils.h"
#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>

namespace stonky::lighter {
using onLogMessage = std::function<void(LogSeverity, const std::string &)>;

/// Raw inbound data message (subscribed/*, update/*). The stream manager
/// dispatches on the message's "type" and "channel".
using onDataEvent = std::function<void(const nlohmann::json &)>;

/**
 * One TLS WebSocket connection to the Lighter stream (wss://<host>/stream).
 *
 * Architecture mirrors the proven Bybit/MEXC sessions: outbound messages
 * (subscribe, pong) go through an internal write pump so they are sent even on a
 * quiet stream; every completion handler is generation-guarded; on any transport
 * error or inbound-silence timeout the socket is torn down and reconnected with
 * exponential backoff (1 s → 30 s) and all subscriptions are replayed. Only
 * close() stops the loop.
 *
 * Lighter needs no auth — account data is public by account index. Subscriptions
 * are held back until the venue's `{"type":"connected"}` greeting, then flushed;
 * the venue drives keepalive with `{"type":"ping"}`, answered with a JSON pong.
 */
class WebSocketSession final : public std::enable_shared_from_this<WebSocketSession> {
    struct P;
    std::unique_ptr<P> m_p;

public:
    WebSocketSession(boost::asio::io_context &ioc, boost::asio::ssl::context &ctx, const onLogMessage &onLogMessageCB);

    ~WebSocketSession();

    /**
     * Run the session.
     * @param host e.g. mainnet.zklighter.elliot.ai
     * @param port e.g. 443
     * @param path WebSocket upgrade path, e.g. /stream
     * @param dataEventCB inbound data-message callback — invoked on the io thread
     */
    void run(const std::string &host, const std::string &port, const std::string &path, const onDataEvent &dataEventCB);

    /// Close asynchronously and disable automatic reconnect.
    void close() const;

    /**
     * Subscribe to a channel. Safe from any thread; queued until the connection
     * is up and the venue greeting arrived.
     * @param channel e.g. "order_book/1" or "account_all/42"
     */
    void subscribe(const std::string &channel) const;

    /// Unsubscribe a channel previously passed to subscribe(). No-op otherwise.
    void unsubscribe(const std::string &channel) const;

    /// True once transport is up and the venue greeting was received.
    [[nodiscard]] bool isConnected() const;

    /// True if the channel is subscribed (sent or pending).
    [[nodiscard]] bool isSubscribed(const std::string &channel) const;
};
} // namespace stonky::lighter

#endif // INCLUDE_STONKY_LIGHTER_WS_SESSION_H
