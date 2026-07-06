/**
Lighter WebSocket Stream Manager

Licensed under the MIT License <http://opensource.org/licenses/MIT>.
SPDX-License-Identifier: MIT
Copyright (c) 2026 Vitezslav Kot <vitezslav.kot@stonky.cz>, Stonky s.r.o.
*/

#include "stonky/lighter/lighter_ws_stream_manager.h"
#include "stonky/lighter/tls_verify.h"
#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <fmt/format.h>
#include <atomic>
#include <map>
#include <mutex>
#include <thread>

namespace stonky::lighter {
namespace {
constexpr auto WS_HOST = "mainnet.zklighter.elliot.ai";
constexpr auto WS_PORT = "443";
constexpr auto WS_PATH = "/stream";

/// Parse the id after the channel separator; responses use "order_book:1" while
/// subscribes use "order_book/1", so accept both.
int channelId(const std::string &channel) {
    const auto pos = channel.find_first_of(":/");
    if (pos == std::string::npos) {
        return -1;
    }

    try {
        return std::stoi(channel.substr(pos + 1));
    } catch (...) {
        return -1;
    }
}

double toDouble(const nlohmann::json &v) {
    if (v.is_string()) {
        try {
            return std::stod(v.get<std::string>());
        } catch (...) {
            return 0.0;
        }
    }

    return v.is_number() ? v.get<double>() : 0.0;
}
} // namespace

struct WSStreamManager::P {
    boost::asio::io_context ioc;
    boost::asio::ssl::context ctx{boost::asio::ssl::context::sslv23_client};
    std::shared_ptr<WebSocketSession> session;
    std::thread ioThread;
    std::atomic<bool> running{false};

    std::string host{WS_HOST};
    std::string port{WS_PORT};
    std::string path{WS_PATH};

    onLogMessage logCB;
    onOrderBookUpdate bookCB;
    onAccountUpdate accountCB;
    onAuthTokenProvider authTokenProvider;

    /// Per-market order book (bids descending, asks ascending) — maintained from
    /// snapshot + deltas on the io thread only, so no lock is needed.
    struct Book {
        std::map<double, double, std::greater<>> bids;
        std::map<double, double> asks;
    };

    std::map<int, Book> books;

    /// Guards the lazy session/io-thread creation (subscribe* can be called from
    /// multiple threads before start()).
    std::recursive_mutex clientLocker;

    void log(const LogSeverity sev, const std::string &msg) const {
        if (logCB) {
            logCB(sev, msg);
        }
    }

    static void applyLevels(std::map<double, double, std::greater<>> &side, const nlohmann::json &levels) {
        for (const auto &lvl: levels) {
            const double price = toDouble(lvl.value("price", nlohmann::json(0)));
            const double size = toDouble(lvl.value("size", nlohmann::json(0)));
            if (price <= 0.0) {
                continue;
            }
            if (size == 0.0) {
                side.erase(price);
            } else {
                side[price] = size;
            }
        }
    }

    static void applyLevels(std::map<double, double> &side, const nlohmann::json &levels) {
        for (const auto &lvl: levels) {
            const double price = toDouble(lvl.value("price", nlohmann::json(0)));
            const double size = toDouble(lvl.value("size", nlohmann::json(0)));
            if (price <= 0.0) {
                continue;
            }
            if (size == 0.0) {
                side.erase(price);
            } else {
                side[price] = size;
            }
        }
    }

    void onOrderBookMessage(const int marketId, const nlohmann::json &orderBook, const bool snapshot) {
        Book &book = books[marketId];

        if (snapshot) {
            book.bids.clear();
            book.asks.clear();
        }

        if (const auto it = orderBook.find("bids"); it != orderBook.end() && it->is_array()) {
            applyLevels(book.bids, *it);
        }

        if (const auto it = orderBook.find("asks"); it != orderBook.end() && it->is_array()) {
            applyLevels(book.asks, *it);
        }

        const double bestBid = book.bids.empty() ? 0.0 : book.bids.begin()->first;
        const double bestAsk = book.asks.empty() ? 0.0 : book.asks.begin()->first;

        // A crossed local book means a lost/misapplied delta — the consumer's
        // sanity gate would just silently stall on it, so make it loud and force
        // a fresh snapshot via resubscribe.
        if (bestBid > 0.0 && bestAsk > 0.0 && bestBid > bestAsk) {
            log(LogSeverity::Warning, fmt::format("Lighter WS: crossed local book market {} (bid {} > ask {}), resubscribing", marketId, bestBid, bestAsk));
            if (session) {
                const auto channel = fmt::format("order_book/{}", marketId);
                session->unsubscribe(channel);
                session->subscribe(channel);
            }
            return;
        }

        if (bookCB) {
            bookCB(marketId, bestBid, bestAsk);
        }
    }

    void onMessage(const nlohmann::json &json) {
        const auto typeIt = json.find("type");
        if (typeIt == json.end() || !typeIt->is_string()) {
            return;
        }

        const auto type = typeIt->get<std::string>();
        std::string channel;
        if (const auto it = json.find("channel"); it != json.end() && it->is_string()) {
            channel = it->get<std::string>();
        }

        if (type == "subscribed/order_book" || type == "update/order_book") {
            if (const auto it = json.find("order_book"); it != json.end() && it->is_object()) {
                onOrderBookMessage(channelId(channel), *it, type == "subscribed/order_book");
            }
        } else if (type == "subscribed/account_all_orders" || type == "update/account_all_orders" || type == "subscribed/account_all" || type == "update/account_all") {
            // account_all_orders carries the order stream (auth channel);
            // account_all is positions/balances only — forwarded for completeness.
            if (accountCB) {
                accountCB(json);
            }
        } else if (type.starts_with("error")) {
            log(LogSeverity::Error, fmt::format("Lighter WS: {}", json.dump()));
        }
    }

    void ensureSession() {
        std::lock_guard lk(clientLocker);

        if (session) {
            return;
        }

        // Configure the SSL context BEFORE the session/io thread exists — a
        // concurrent set_default_verify_paths during the handshake is a data
        // race on the shared X509_STORE.
        enableTlsPeerVerification(ctx);

        session = std::make_shared<WebSocketSession>(ioc, ctx, logCB);

        if (authTokenProvider) {
            session->setAuthTokenProvider(authTokenProvider);
        }

        session->run(host, port, path, [this](const nlohmann::json &json) { onMessage(json); });

        if (!running.exchange(true)) {
            ioThread = std::thread([this] {
                for (;;) {
                    try {
                        if (ioc.stopped()) {
                            ioc.restart();
                        }
                        ioc.run();
                        break;
                    } catch (std::exception &e) {
                        log(LogSeverity::Error, fmt::format("Lighter WS io: {}", e.what()));
                    }
                }
            });
        }
    }
};

WSStreamManager::WSStreamManager() : m_p(std::make_unique<P>()) {}

WSStreamManager::~WSStreamManager() {
    if (m_p->session) {
        m_p->session->close();
    }

    m_p->ioc.stop();

    if (m_p->ioThread.joinable()) {
        m_p->ioThread.join();
    }
}

void WSStreamManager::setLoggerCallback(const onLogMessage &cb) const { m_p->logCB = cb; }

void WSStreamManager::setOrderBookCallback(const onOrderBookUpdate &cb) const { m_p->bookCB = cb; }

void WSStreamManager::setAccountCallback(const onAccountUpdate &cb) const { m_p->accountCB = cb; }

void WSStreamManager::setEndpoint(const std::string &host, const std::string &port, const std::string &path) const {
    m_p->host = host;
    m_p->port = port;
    m_p->path = path;
}

void WSStreamManager::start() const {
    m_p->ensureSession();
}

void WSStreamManager::subscribeOrderBook(const int marketId) const {
    m_p->ensureSession();
    m_p->session->subscribe(fmt::format("order_book/{}", marketId));
}

void WSStreamManager::unsubscribeOrderBook(const int marketId) const {
    if (m_p->session) {
        m_p->session->unsubscribe(fmt::format("order_book/{}", marketId));
    }
}

void WSStreamManager::subscribeAccount(const int accountIndex) const {
    m_p->ensureSession();
    m_p->session->subscribe(fmt::format("account_all/{}", accountIndex));
}

void WSStreamManager::subscribeAccountOrders(const int accountIndex, const onAuthTokenProvider &tokenProvider) const {
    {
        std::lock_guard lk(m_p->clientLocker);
        m_p->authTokenProvider = tokenProvider;
    }

    m_p->ensureSession();
    m_p->session->setAuthTokenProvider(tokenProvider);
    m_p->session->subscribe(fmt::format("account_all_orders/{}", accountIndex));
}

bool WSStreamManager::isConnected() const { return m_p->session && m_p->session->isConnected(); }
} // namespace stonky::lighter
