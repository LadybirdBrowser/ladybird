/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/JsonObject.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibHTTP/Header.h>
#include <LibURL/URL.h>
#include <LibWebCommon/Export.h>
#include <LibWebCommon/WebDriver/Error.h>

namespace Web::WebDriver::Network {

// https://w3c.github.io/webdriver-bidi/#type-network-UrlPattern
struct WEBCOMMON_API UrlPattern {
    Optional<String> protocol;
    Optional<String> hostname;
    Optional<String> port;
    Optional<String> pathname;
    Optional<String> search;

    static ErrorOr<UrlPattern, Error> parse(JsonValue const&);
    bool matches(URL::URL const&) const;
};

using Header = HTTP::Header;

// The parameter of a network event in which the process that reported it names the network interception that blocks
// the event's request, or that the event ended.
constexpr auto network_interception_parameter = "__webdriver_network_interception"sv;

// The event in which the process that hosts a blocked request reports that the request's network interception ended.
constexpr auto network_interception_ended_event = "network.__webdriverInterceptionEnded"sv;

WEBCOMMON_API ErrorOr<Vector<Header>, Error> create_a_headers_list(JsonValue const&);

struct WEBCOMMON_API Intercept {
    String id;
    Vector<UrlPattern> url_patterns;
    Vector<String> phases;
    Optional<Vector<String>> browsing_contexts;
};

// NB: Parse all overrides before changing a blocked request or response.
struct WEBCOMMON_API InterceptionOverrides {
    Optional<URL::URL> url;
    Optional<ByteString> method;
    Optional<Vector<Header>> headers;
    Optional<ByteBuffer> body;
    Optional<ByteString> cookie_header;
    Optional<Vector<ByteString>> set_cookie_values;
    Optional<u64> status;
    Optional<ByteString> reason;
};
WEBCOMMON_API ErrorOr<InterceptionOverrides, Error> deserialize_interception_overrides(StringView method, JsonObject const&);

// NB: Validate the remote end definition before looking up the blocked request. The command's remote end steps
//     perform any additional validation of the overrides after that lookup.
WEBCOMMON_API ErrorOr<void, Error> validate_interception_parameters(StringView method, JsonObject const&);

}
