/**
Lighter WebSocket Session

Licensed under the MIT License <http://opensource.org/licenses/MIT>.
SPDX-License-Identifier: MIT
Copyright (c) 2026 Vitezslav Kot <vitezslav.kot@stonky.cz>, Stonky s.r.o.
*/

#include "stonky/lighter/lighter_ws_session.h"
#include <fmt/format.h>
#include <boost/asio/post.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/version.hpp>
#include <boost/beast/websocket.hpp>
#include <openssl/err.h>
#include <algorithm>
#include <atomic>
#include <list>
#include <ranges>

#ifndef MAKE_FILELINE
#define STONKY_STRINGIZE_I(x) #x
#define STONKY_STRINGIZE(x) STONKY_STRINGIZE_I(x)
#define MAKE_FILELINE __FILE__ "(" STONKY_STRINGIZE(__LINE__) ")"
#endif

namespace stonky::lighter {
static constexpr int PING_INTERVAL_IN_S = 15;
/// Reconnect if the venue sends nothing (updates or pings) for this long.
static constexpr int ACTIVITY_TIMEOUT_IN_S = 45;
static constexpr int RECONNECT_DELAY_MAX_IN_S = 30;

using WsStream = boost::beast::websocket::stream<boost::beast::ssl_stream<boost::beast::tcp_stream>>;

struct WebSocketSession::P {
    boost::asio::io_context &ioc;
    boost::asio::ssl::context &ctx;
    boost::asio::strand<boost::asio::io_context::executor_type> strand;
    boost::asio::ip::tcp::resolver resolver;
    std::shared_ptr<WsStream> ws;
    boost::beast::multi_buffer buffer;

    std::string host;
    std::string port;
    std::string path{"/stream"};
    std::string hostHeader;

    onLogMessage logMessageCB;
    onDataEvent dataEventCB;

    /// Outbound write pump: one async_write in flight; subscribe/pong ops go
    /// through the queue so writes never depend on inbound traffic or race.
    std::list<std::string> outboundQueue;
    bool writeInFlight = false;
    std::string writeBuffer;

    /// Confirmed (sent) subscriptions and pending ones. On reconnect all
    /// subscriptions move back to pending and are replayed after the greeting.
    std::vector<std::string> subscriptions;
    std::list<std::string> pendingTopics;
    mutable std::recursive_mutex locker;

    boost::asio::steady_timer pingTimer;
    boost::asio::steady_timer reconnectTimer;
    int reconnectDelayS = 1;
    int generation = 0;
    bool connected = false; ///< transport up (WS handshake done)
    bool ready = false;     ///< venue "connected" greeting received → may subscribe
    std::atomic<bool> connectedFlag{false};
    bool reconnectScheduled = false;
    bool userClosed = false;
    std::chrono::steady_clock::time_point lastActivity{};

    P(boost::asio::io_context &ioContext, boost::asio::ssl::context &sslCtx, const onLogMessage &onLogMessageCB) :
        ioc(ioContext), ctx(sslCtx), strand(boost::asio::make_strand(ioContext)), resolver(strand), logMessageCB(onLogMessageCB), pingTimer(strand),
        reconnectTimer(strand) {}

    void log(const LogSeverity severity, const std::string &message) const {
        if (logMessageCB) {
            logMessageCB(severity, message);
        }
    }

    [[nodiscard]] bool isUserClosed() const {
        std::lock_guard lk(locker);
        return userClosed;
    }

    // ── Outbound write pump ─────────────────────────────────────────

    void enqueueOp(const std::string &payload) {
        std::lock_guard lk(locker);
        outboundQueue.push_back(payload);
    }

    void pump(const std::shared_ptr<WebSocketSession> &self) {
        if (writeInFlight || !connected || !ws || !ws->is_open()) {
            return;
        }

        {
            std::lock_guard lk(locker);

            if (outboundQueue.empty()) {
                return;
            }

            writeBuffer = std::move(outboundQueue.front());
            outboundQueue.pop_front();
        }

        writeInFlight = true;
        ws->async_write(boost::asio::buffer(writeBuffer), [this, self, gen = generation, wsRef = ws](const boost::beast::error_code &ec, const std::size_t bytes) {
            boost::ignore_unused(bytes, wsRef);

            if (gen != generation) {
                return;
            }

            writeInFlight = false;

            if (ec) {
                return handleError(self, gen, fmt::format("{}: write: {}", MAKE_FILELINE, ec.message()));
            }

            pump(self);
        });
    }

    // ── Subscription bookkeeping ────────────────────────────────────

    onAuthTokenProvider authTokenProvider;

    /// Built per flush, not cached: auth channels get a FRESH token each time so
    /// a reconnect replay never sends an expired one.
    [[nodiscard]] std::string subscribeMsg(const std::string &channel) const {
        nlohmann::json msg{{"type", "subscribe"}, {"channel", channel}};

        if (authTokenProvider) {
            if (const auto token = authTokenProvider(channel); !token.empty()) {
                msg["auth"] = token;
            }
        }

        return msg.dump();
    }

    static std::string unsubscribeMsg(const std::string &channel) { return nlohmann::json{{"type", "unsubscribe"}, {"channel", channel}}.dump(); }

    void addTopic(const std::string &channel) {
        std::lock_guard lk(locker);

        if (std::ranges::find(subscriptions, channel) != subscriptions.end() || std::ranges::find(pendingTopics, channel) != pendingTopics.end()) {
            return;
        }

        pendingTopics.push_back(channel);
    }

    bool removeTopic(const std::string &channel) {
        std::lock_guard lk(locker);
        std::erase(pendingTopics, channel);

        if (const auto it = std::ranges::find(subscriptions, channel); it != subscriptions.end()) {
            subscriptions.erase(it);
            return true;
        }

        return false;
    }

    [[nodiscard]] bool isSubscribed(const std::string &channel) const {
        std::lock_guard lk(locker);
        return std::ranges::find(subscriptions, channel) != subscriptions.end() || std::ranges::find(pendingTopics, channel) != pendingTopics.end();
    }

    /// Send each pending subscription (one channel per message). No-op until the
    /// venue greeting arrives.
    void flushTopics(const std::shared_ptr<WebSocketSession> &self) {
        if (!connected || !ready) {
            return;
        }

        std::vector<std::string> toSend;
        {
            std::lock_guard lk(locker);

            for (auto &channel: pendingTopics) {
                subscriptions.push_back(channel);
                toSend.push_back(channel);
            }

            pendingTopics.clear();
        }

        for (const auto &channel: toSend) {
            enqueueOp(subscribeMsg(channel));
        }

        pump(self);
    }

    // ── Connect chain ───────────────────────────────────────────────

    void startConnect(const std::shared_ptr<WebSocketSession> &self) {
        if (isUserClosed()) {
            return;
        }

        ++generation;
        const int gen = generation;

        connected = false;
        ready = false;
        connectedFlag = false;
        writeInFlight = false;
        hostHeader = host;
        buffer.consume(buffer.size());
        ws = std::make_shared<WsStream>(strand, ctx);
        // Bind the verified peer certificate to the expected hostname (the ctx
        // has verify_peer on; SNI alone authenticates nothing).
        ws->next_layer().set_verify_callback(boost::asio::ssl::host_name_verification(host));

        resolver.async_resolve(host, port, [this, self, gen](const boost::beast::error_code &ec, const boost::asio::ip::tcp::resolver::results_type &results) {
            if (gen != generation) {
                return;
            }

            onResolve(self, gen, ec, results);
        });
    }

    void onResolve(const std::shared_ptr<WebSocketSession> &self, const int gen, const boost::beast::error_code &ec,
                   const boost::asio::ip::tcp::resolver::results_type &results) {
        if (ec) {
            return handleError(self, gen, fmt::format("{}: {}", MAKE_FILELINE, ec.message()));
        }

        get_lowest_layer(*ws).expires_after(std::chrono::seconds(30));
        get_lowest_layer(*ws).async_connect(results, [this, self, gen, wsRef = ws](const boost::beast::error_code &e,
                                                                                    const boost::asio::ip::tcp::resolver::results_type::endpoint_type &ep) {
            boost::ignore_unused(wsRef);

            if (gen != generation) {
                return;
            }

            onConnect(self, gen, e, ep);
        });
    }

    void onConnect(const std::shared_ptr<WebSocketSession> &self, const int gen, boost::beast::error_code ec,
                   const boost::asio::ip::tcp::resolver::results_type::endpoint_type &ep) {
        if (ec) {
            return handleError(self, gen, fmt::format("{}: {}", MAKE_FILELINE, ec.message()));
        }

        get_lowest_layer(*ws).expires_after(std::chrono::seconds(30));

        if (!SSL_set_tlsext_host_name(ws->next_layer().native_handle(), host.c_str())) {
            ec = boost::beast::error_code(static_cast<int>(ERR_get_error()), boost::asio::error::get_ssl_category());
            return handleError(self, gen, fmt::format("{}: {}", MAKE_FILELINE, ec.message()));
        }

        hostHeader = host + ':' + std::to_string(ep.port());

        ws->next_layer().async_handshake(boost::asio::ssl::stream_base::client, [this, self, gen, wsRef = ws](const boost::beast::error_code &e) {
            boost::ignore_unused(wsRef);

            if (gen != generation) {
                return;
            }

            onSSLHandshake(self, gen, e);
        });
    }

    void onSSLHandshake(const std::shared_ptr<WebSocketSession> &self, const int gen, const boost::beast::error_code &ec) {
        if (ec) {
            return handleError(self, gen, fmt::format("{}: {}", MAKE_FILELINE, ec.message()));
        }

        get_lowest_layer(*ws).expires_never();
        ws->set_option(boost::beast::websocket::stream_base::timeout::suggested(boost::beast::role_type::client));
        ws->set_option(boost::beast::websocket::stream_base::decorator(
                [](boost::beast::websocket::request_type &req) { req.set(boost::beast::http::field::user_agent, std::string(BOOST_BEAST_VERSION_STRING) + " lighter-client"); }));

        ws->async_handshake(hostHeader, path, [this, self, gen, wsRef = ws](const boost::beast::error_code &e) {
            boost::ignore_unused(wsRef);

            if (gen != generation) {
                return;
            }

            onHandshake(self, gen, e);
        });
    }

    void onHandshake(const std::shared_ptr<WebSocketSession> &self, const int gen, const boost::beast::error_code &ec) {
        if (ec) {
            return handleError(self, gen, fmt::format("{}: {}", MAKE_FILELINE, ec.message()));
        }

        if (isUserClosed()) {
            return;
        }

        connected = true;
        lastActivity = std::chrono::steady_clock::now();

        pingTimer.expires_after(boost::asio::chrono::seconds(PING_INTERVAL_IN_S));
        pingTimer.async_wait([this, self, gen](const boost::beast::error_code &e) { onPingTimer(self, gen, e); });

        // Subscriptions are held until the venue "connected" greeting.
        ws->async_read(buffer, [this, self, gen, wsRef = ws](const boost::beast::error_code &e, const std::size_t transferred) {
            boost::ignore_unused(wsRef);

            if (gen != generation) {
                return;
            }

            onRead(self, gen, e, transferred);
        });
    }

    // ── Inbound ─────────────────────────────────────────────────────

    void onRead(const std::shared_ptr<WebSocketSession> &self, const int gen, const boost::beast::error_code &ec, std::size_t bytesTransferred) {
        boost::ignore_unused(bytesTransferred);

        if (ec) {
            return handleError(self, gen, fmt::format("{}: {}", MAKE_FILELINE, ec.message()));
        }

        lastActivity = std::chrono::steady_clock::now();

        try {
            std::string strBuffer;
            strBuffer.reserve(buffer.size());

            for (const auto &it: buffer.data()) {
                strBuffer.append(static_cast<const char *>(it.data()), it.size());
            }

            buffer.consume(buffer.size());

            if (const nlohmann::json json = nlohmann::json::parse(strBuffer); json.is_object()) {
                dispatch(self, json);
            }

            if (gen != generation || reconnectScheduled || !connected) {
                return;
            }

            if (ready) {
                reconnectDelayS = 1;
            }

            ws->async_read(buffer, [this, self, gen, wsRef = ws](const boost::beast::error_code &e, const std::size_t transferred) {
                boost::ignore_unused(wsRef);

                if (gen != generation) {
                    return;
                }

                onRead(self, gen, e, transferred);
            });
        } catch (nlohmann::json::exception &exc) {
            handleError(self, gen, fmt::format("{}: {}", MAKE_FILELINE, exc.what()));
        }
    }

    void dispatch(const std::shared_ptr<WebSocketSession> &self, const nlohmann::json &json) {
        const auto typeIt = json.find("type");
        const std::string type = typeIt != json.end() && typeIt->is_string() ? typeIt->get<std::string>() : "";

        if (type == "connected") {
            ready = true;
            connectedFlag = true;
            reconnectDelayS = 1;
            log(LogSeverity::Info, "Lighter WS connected");
            flushTopics(self);
            return;
        }

        if (type == "ping") {
            enqueueOp(nlohmann::json{{"type", "pong"}}.dump());
            pump(self);
            return;
        }

        // Data message (subscribed/*, update/*, error, ...) → the consumer.
        if (dataEventCB) {
            try {
                dataEventCB(json);
            } catch (std::exception &e) {
                log(LogSeverity::Error, fmt::format("{}: {}", MAKE_FILELINE, e.what()));
            }
        }
    }

    // ── Heartbeat watchdog ──────────────────────────────────────────

    void onPingTimer(const std::shared_ptr<WebSocketSession> &self, const int gen, const boost::beast::error_code &ec) {
        if (ec || gen != generation || isUserClosed()) {
            return;
        }

        if (connected) {
            if (const auto silent = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - lastActivity).count(); silent > ACTIVITY_TIMEOUT_IN_S) {
                return handleError(self, gen, fmt::format("{}: no activity for {} s", MAKE_FILELINE, silent));
            }
        }

        pingTimer.expires_after(boost::asio::chrono::seconds(PING_INTERVAL_IN_S));
        pingTimer.async_wait([this, self, gen](const boost::beast::error_code &e) { onPingTimer(self, gen, e); });
    }

    // ── Error / reconnect / close ───────────────────────────────────

    void handleError(const std::shared_ptr<WebSocketSession> &self, const int gen, const std::string &message) {
        if (gen != generation || reconnectScheduled) {
            return;
        }

        log(LogSeverity::Error, message);

        if (isUserClosed()) {
            return;
        }

        reconnectScheduled = true;
        connected = false;
        ready = false;
        connectedFlag = false;
        writeInFlight = false;
        pingTimer.cancel();

        {
            std::lock_guard lk(locker);
            outboundQueue.clear();

            for (auto &subscription: std::ranges::reverse_view(subscriptions)) {
                pendingTopics.push_front(subscription);
            }

            subscriptions.clear();
        }

        if (ws) {
            boost::beast::error_code ignored;
            get_lowest_layer(*ws).socket().close(ignored);
        }

        log(LogSeverity::Warning, fmt::format("Lighter WS reconnecting in {} s", reconnectDelayS));
        reconnectTimer.expires_after(boost::asio::chrono::seconds(reconnectDelayS));
        reconnectDelayS = std::min(reconnectDelayS * 2, RECONNECT_DELAY_MAX_IN_S);

        reconnectTimer.async_wait([this, self, gen](const boost::beast::error_code &e) {
            if (gen != generation) {
                return;
            }

            reconnectScheduled = false;

            if (e || isUserClosed()) {
                return;
            }

            startConnect(self);
        });
    }

    void closeByUser() {
        {
            std::lock_guard lk(locker);

            if (userClosed) {
                return;
            }

            userClosed = true;
        }

        pingTimer.cancel();
        reconnectTimer.cancel();
        resolver.cancel();

        if (!ws) {
            return;
        }

        if (ws->is_open() && !writeInFlight) {
            ws->async_close(boost::beast::websocket::close_code::normal, [this, wsRef = ws](const boost::beast::error_code &ec) {
                boost::ignore_unused(wsRef);

                if (ec) {
                    log(LogSeverity::Error, fmt::format("{}: {}", MAKE_FILELINE, ec.message()));
                }
            });
        } else {
            boost::beast::error_code ignored;
            get_lowest_layer(*ws).socket().close(ignored);
        }
    }
};

WebSocketSession::WebSocketSession(boost::asio::io_context &ioc, boost::asio::ssl::context &ctx, const onLogMessage &onLogMessageCB) :
    m_p(std::make_unique<P>(ioc, ctx, onLogMessageCB)) {}

WebSocketSession::~WebSocketSession() = default;

void WebSocketSession::subscribe(const std::string &channel) const {
    m_p->addTopic(channel);

    auto self = const_cast<WebSocketSession *>(this)->shared_from_this();
    post(m_p->strand, [self] { self->m_p->flushTopics(self); });
}

void WebSocketSession::unsubscribe(const std::string &channel) const {
    if (!m_p->removeTopic(channel)) {
        return;
    }

    auto self = const_cast<WebSocketSession *>(this)->shared_from_this();
    post(m_p->strand, [self, channel] {
        if (!self->m_p->connected) {
            return;
        }

        self->m_p->enqueueOp(P::unsubscribeMsg(channel));
        self->m_p->pump(self);
    });
}

void WebSocketSession::setAuthTokenProvider(const onAuthTokenProvider &cb) const { m_p->authTokenProvider = cb; }

bool WebSocketSession::isConnected() const { return m_p->connectedFlag.load(); }

bool WebSocketSession::isSubscribed(const std::string &channel) const { return m_p->isSubscribed(channel); }

void WebSocketSession::run(const std::string &host, const std::string &port, const std::string &path, const onDataEvent &dataEventCB) {
    m_p->host = host;
    m_p->port = port;
    m_p->path = path;
    m_p->dataEventCB = dataEventCB;

    auto self = shared_from_this();
    post(m_p->strand, [self] { self->m_p->startConnect(self); });
}

void WebSocketSession::close() const {
    auto self = const_cast<WebSocketSession *>(this)->shared_from_this();
    post(m_p->strand, [self] { self->m_p->closeByUser(); });
}
} // namespace stonky::lighter
