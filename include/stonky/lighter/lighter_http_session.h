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
     */
    [[nodiscard]] http::response<http::string_body> get(const std::string& path, const nlohmann::json& query) const;
};
} // namespace stonky::lighter

#endif // INCLUDE_STONKY_LIGHTER_HTTP_SESSION_H
