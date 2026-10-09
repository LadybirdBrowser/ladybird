/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibHTTP/Port.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/PortBlocking.h>
#include <LibWeb/Fetch/Infrastructure/URL.h>

namespace Web::Fetch::Infrastructure {

// https://fetch.spec.whatwg.org/#block-bad-port
RequestOrResponseBlocking block_bad_port(Request const& request)
{
    // 1. Let url be request’s current URL.
    auto const& url = request.current_url();

    // 2. If url’s scheme is an HTTP(S) scheme and url’s port is a bad port, then return blocked.
    if (is_http_or_https_scheme(url.scheme()) && url.port().has_value() && HTTP::is_bad_port(*url.port()))
        return RequestOrResponseBlocking::Blocked;

    // 3. Return allowed.
    return RequestOrResponseBlocking::Allowed;
}

}
