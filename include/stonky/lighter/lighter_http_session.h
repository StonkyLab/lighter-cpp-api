/**
Lighter HTTPS Session

Licensed under the MIT License <http://opensource.org/licenses/MIT>.
SPDX-License-Identifier: MIT
Copyright (c) 2026 Vitezslav Kot <vitezslav.kot@stonky.cz>, Stonky s.r.o.
*/

#ifndef INCLUDE_STONKY_LIGHTER_HTTP_SESSION_H
#define INCLUDE_STONKY_LIGHTER_HTTP_SESSION_H

#include <boost/asio/connect.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <string>
#include <nlohmann/json_fwd.hpp>

namespace stonky::lighter {
namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;

class HTTPSession {
    struct P;
    std::unique_ptr<P> m_p{};

public:
    explicit HTTPSession(const std::string& host = "");

    ~HTTPSession();

    /**
     * Issue a GET request. The `query` JSON object is flattened into a query string
     * ("key=value&key=value&..."); pass an empty object for no params.
     * If `authHeader` is non-empty it is sent as the `Authorization` header value
     * (Lighter expects a read-only auth token formatted as `ro:account:scope:expiry:nonce`).
     */
    [[nodiscard]] http::response<http::string_body> get(const std::string& path, const nlohmann::json& query,
                                                        const std::string& authHeader = "") const;

    /**
     * Issue a POST request with an `application/x-www-form-urlencoded` body
     * (Lighter's write endpoints — sendTx — take form params). The caller passes
     * the already url-encoded body ("k=v&k=v"). authHeader is optional.
     */
    [[nodiscard]] http::response<http::string_body> postForm(const std::string& path, const std::string& formBody,
                                                             const std::string& authHeader = "") const;
};
} // namespace stonky::lighter

#endif // INCLUDE_STONKY_LIGHTER_HTTP_SESSION_H
