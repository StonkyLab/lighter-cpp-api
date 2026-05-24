/**
Lighter HTTPS Session

Licensed under the MIT License <http://opensource.org/licenses/MIT>.
SPDX-License-Identifier: MIT
Copyright (c) 2026 Vitezslav Kot <vitezslav.kot@stonky.cz>, Stonky s.r.o.
*/

#include "stonky/lighter/lighter_http_session.h"
#include "stonky/utils/json_utils.h"
#include "nlohmann/json.hpp"
#include <boost/asio/ssl.hpp>
#include <boost/beast/version.hpp>
#include <openssl/err.h>

namespace stonky::lighter {
namespace ssl = boost::asio::ssl;
using tcp = net::ip::tcp;

constexpr auto API_URI = "mainnet.zklighter.elliot.ai";

// NOTE: the API is fronted by AWS WAF on CloudFront. The /orderBookDetails endpoint passes
// from Beast reliably, but /candles intermittently returns HTTP 405 + a CAPTCHA challenge
// page (`x-amzn-waf-action: captcha`) — likely a TLS-fingerprint (JA3) heuristic that
// distinguishes Beast's OpenSSL ClientHello from curl/browsers. Mitigations if it becomes a
// blocker for sustained backfill: (1) swap this session for a libcurl-based one (different
// TLS fingerprint that the WAF currently lets through), (2) acquire a WAF-bypass cookie via
// a browser challenge and inject it here, or (3) request whitelisting from Lighter. None of
// these are warranted until the user actually hits the limit in production.

struct HTTPSession::P {
    net::io_context ioc;
    std::string uri;

    http::response<http::string_body> request(http::request<http::string_body> req);
};

HTTPSession::HTTPSession(const std::string& host) : m_p(std::make_unique<P>()) {
    m_p->uri = host.empty() ? API_URI : host;
}

HTTPSession::~HTTPSession() = default;

http::response<http::string_body> HTTPSession::get(const std::string& path, const nlohmann::json& query) const {
    std::string target = path;
    if (!query.is_null() && query.is_object() && !query.empty()) {
        target += "?";
        target += queryStringFromJson(query);
    }
    http::request<http::string_body> req{http::verb::get, target, 11};
    // Mimic curl's minimal header set; the AWS WAF in front of mainnet.zklighter.elliot.ai
    // is sensitive to header fingerprinting and "Accept: application/json" plus a
    // non-curl User-Agent occasionally trips the bot challenge.
    req.set(http::field::accept, "*/*");
    return m_p->request(req);
}

http::response<http::string_body> HTTPSession::P::request(http::request<http::string_body> req) {
    req.set(http::field::host, uri);
    // AWS WAF in front of mainnet.zklighter.elliot.ai intermittently CAPTCHAs requests
    // with curl-like or default Beast user-agents. A browser-shaped UA passes reliably.
    req.set(http::field::user_agent,
            "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
            "Chrome/131.0.0.0 Safari/537.36");

    ssl::context ctx{ssl::context::sslv23_client};
    ctx.set_default_verify_paths();

    tcp::resolver resolver{ioc};
    ssl::stream<tcp::socket> stream{ioc, ctx};

    if (!SSL_set_tlsext_host_name(stream.native_handle(), uri.c_str())) {
        boost::system::error_code ec{static_cast<int>(ERR_get_error()), net::error::get_ssl_category()};
        throw boost::system::system_error{ec};
    }

    auto const results = resolver.resolve(uri, "443");
    net::connect(stream.next_layer(), results.begin(), results.end());
    stream.handshake(ssl::stream_base::client);

    http::write(stream, req);
    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    http::read(stream, buffer, response);

    boost::system::error_code ec;
    [[maybe_unused]] const auto rc = stream.shutdown(ec);
    if (ec == boost::asio::error::eof) {
        ec.assign(0, ec.category());
    }

    return response;
}
} // namespace stonky::lighter
