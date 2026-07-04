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

// NOTE: the API is fronted by AWS WAF on CloudFront with a rate-based rule. Individual
// requests from Beast pass reliably — verified against /candles, /fundings and
// /orderBookDetails with a handshake byte-identical to curl's (same system OpenSSL,
// TLS 1.3, HTTP/1.1, minimal headers). The HTTP 405 + CAPTCHA "Human Verification" page
// (`x-amzn-waf-action: captcha`) that once looked like a TLS-fingerprint (JA3) block was
// in fact self-inflicted: a pagination bug in RESTClient::getHistoricalPrices spun forever
// and flooded this endpoint until the rate limiter tripped (fixed 2026-05-30). This is NOT
// a transport/fingerprint problem — do NOT "fix" it by rewriting this session on libcurl.
// If a genuine sustained-throughput WAF limit ever appears, throttle the request rate first.

struct HTTPSession::P {
    net::io_context ioc;
    std::string uri;

    http::response<http::string_body> request(http::request<http::string_body> req);
};

HTTPSession::HTTPSession(const std::string& host) : m_p(std::make_unique<P>()) {
    m_p->uri = host.empty() ? API_URI : host;
}

HTTPSession::~HTTPSession() = default;

http::response<http::string_body> HTTPSession::get(const std::string& path, const nlohmann::json& query,
                                                    const std::string& authHeader) const {
    std::string target = path;
    if (!query.is_null() && query.is_object() && !query.empty()) {
        target += "?";
        target += queryStringFromJson(query);
    }
    http::request<http::string_body> req{http::verb::get, target, 11};
    // Minimal header set (Accept: */*), mirroring a plain curl request. The WAF does not
    // fingerprint these headers in practice; this just keeps the request shape unremarkable.
    req.set(http::field::accept, "*/*");
    if (!authHeader.empty()) {
        req.set(http::field::authorization, authHeader);
    }
    return m_p->request(req);
}

http::response<http::string_body> HTTPSession::postForm(const std::string& path, const std::string& formBody,
                                                        const std::string& authHeader) const {
    http::request<http::string_body> req{http::verb::post, path, 11};
    req.set(http::field::accept, "*/*");
    req.set(http::field::content_type, "application/x-www-form-urlencoded");
    if (!authHeader.empty()) {
        req.set(http::field::authorization, authHeader);
    }
    req.body() = formBody;
    req.prepare_payload();
    return m_p->request(req);
}

http::response<http::string_body> HTTPSession::P::request(http::request<http::string_body> req) {
    req.set(http::field::host, uri);
    // Browser-shaped User-Agent, kept only as a cheap precaution against UA-based heuristics.
    // Not strictly required (plain curl/default UAs pass too), but harmless.
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
