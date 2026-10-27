/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <AK/CharacterTypes.h>
#include <AK/JsonArray.h>
#include <AK/StringBuilder.h>
#include <LibURL/Parser.h>
#include <LibWebCommon/WebDriver/BiDi.h>
#include <LibWebCommon/WebDriver/Network.h>

namespace Web::WebDriver::Network {

static Error invalid_argument(StringView message)
{
    return Error::from_code(ErrorCode::InvalidArgument, message);
}

static ErrorOr<String, Error> get_string(JsonObject const& object, StringView name)
{
    auto value = object.get(name);
    if (!value.has_value() || !value->is_string())
        return invalid_argument(MUST(String::formatted("Parameter '{}' must be a string", name)));
    return value->as_string();
}

// https://w3c.github.io/webdriver-bidi/#unescape-url-pattern
static ErrorOr<String, Error> unescape_url_pattern(StringView pattern)
{
    // 1. Let forbidden characters be the set of codepoints «U+0028 ((), U+0029 ()), U+002A (*), U+007B ({), U+007D (})»
    // 2. Let result be the empty string.
    StringBuilder result;

    // 3. Let is escaped character be false.
    bool is_escaped_character = false;

    // 4. For each codepoint in pattern:
    // NB: All special characters are ASCII, so other UTF-8 bytes can be copied unchanged.
    for (auto byte : pattern) {
        // 1. If is escaped character is false:
        if (!is_escaped_character) {
            // 1. If forbidden characters contains codepoint, return error with error code invalid argument.
            if (byte == '(' || byte == ')' || byte == '*' || byte == '{' || byte == '}')
                return invalid_argument("Unescaped character in URL pattern"sv);

            // 2. If codepoint is U+005C (\):
            if (byte == '\\') {
                // 1. Set is escaped character to true.
                // 2. Continue.
                is_escaped_character = true;
                continue;
            }
        }

        // 2. Append codepoint to result.
        result.append(byte);

        // 3. Set is escaped character to false.
        is_escaped_character = false;
    }

    // 5. Return success with data result.
    return TRY(result.to_string());
}

// https://w3c.github.io/webdriver-bidi/#parse-url-pattern
ErrorOr<UrlPattern, Error> UrlPattern::parse(JsonValue const& value)
{
    if (!value.is_object())
        return invalid_argument("URL pattern must be an object"sv);
    auto const& pattern = value.as_object();
    auto type = TRY(get_string(pattern, "type"sv));

    // 1. Let has protocol be true.
    // 2. Let has hostname be true.
    // 3. Let has port be true.
    // 4. Let has pathname be true.
    // 5. Let has search be true.
    bool has_protocol = true;
    bool has_hostname = true;
    bool has_port = true;
    bool has_pathname = true;
    bool has_search = true;
    StringBuilder pattern_url;

    // 6. If pattern matches the network.UrlPatternPattern production:
    if (type == "pattern"sv) {
        // 1. Let pattern url be the empty string.
        auto component = [&](StringView name) -> ErrorOr<String, Error> {
            auto text = TRY(get_string(pattern, name));
            return unescape_url_pattern(text);
        };

        // 2. If pattern contains "protocol":
        has_protocol = pattern.has("protocol"sv);
        if (has_protocol) {
            // 1. If pattern["protocol"] is the empty string, return error with error code invalid argument.
            // 2. Let protocol be the result of trying to unescape URL Pattern with pattern["protocol"].
            if (TRY(get_string(pattern, "protocol"sv)).is_empty())
                return invalid_argument("URL pattern protocol must not be empty"sv);
            auto protocol = TRY(component("protocol"sv));
            // 3. For each codepoint in protocol:
            //     1. If codepoint is not ASCII alphanumeric and «U+002B (+), U+002D (-), U+002E (.)» does not contain codepoint:
            //         1. Return error with error code invalid argument.
            for (auto byte : protocol.bytes()) {
                if (!is_ascii_alphanumeric(byte) && byte != '+' && byte != '-' && byte != '.')
                    return invalid_argument("Invalid URL pattern protocol"sv);
            }
            // 4. Append protocol to pattern url.
            pattern_url.append(protocol);
        } else {
            // 3. Otherwise:
            //     1. Set has protocol to false.
            //     2. Append "http" to pattern url.
            pattern_url.append("http"sv);
        }

        // 4. Let scheme be ASCII lowercase with pattern url.
        auto scheme = pattern_url.string_view().to_ascii_lowercase_string();
        // 5. Append ":" to pattern url.
        pattern_url.append(':');
        // 6. If scheme is special, append "//" to pattern url.
        if (URL::is_special_scheme(scheme))
            pattern_url.append("//"sv);

        // 7. If pattern contains "hostname":
        has_hostname = pattern.has("hostname"sv);
        if (has_hostname) {
            // 1. If pattern["hostname"] is the empty string, return error with error code invalid argument.
            // 2. If scheme is "file" return error with error code invalid argument.
            // 3. Let hostname be the result of trying to unescape URL Pattern with pattern["hostname"].
            if (TRY(get_string(pattern, "hostname"sv)).is_empty() || scheme == "file"sv)
                return invalid_argument("Invalid URL pattern hostname"sv);
            auto hostname = TRY(component("hostname"sv));
            // 4. Let inside brackets be false.
            bool inside_brackets = false;
            // 5. For each codepoint in hostname:
            for (auto byte : hostname.bytes()) {
                // 1. If «U+002F (/), U+003F (?), U+0023 (#)» contains codepoint:
                //     1. Return error with error code invalid argument.
                // 2. If inside brackets is false and codepoint is U+003A (:):
                //     1. Return error with error code invalid argument.
                if (byte == '/' || byte == '?' || byte == '#' || (!inside_brackets && byte == ':'))
                    return invalid_argument("Invalid URL pattern hostname"sv);
                // 3. If codepoint is U+005B ([), set inside brackets to true.
                if (byte == '[')
                    inside_brackets = true;
                // 4. If codepoint is U+005D (]), set inside brackets to false.
                if (byte == ']')
                    inside_brackets = false;
            }
            // 6. Append hostname to pattern url.
            pattern_url.append(hostname);
        } else {
            // 8. Otherwise:
            //     1. If scheme is not "file", append "placeholder" to pattern url.
            //     2. Set has hostname to false.
            if (scheme != "file"sv)
                pattern_url.append("placeholder"sv);
        }

        // 9. If pattern contains "port":
        has_port = pattern.has("port"sv);
        if (has_port) {
            // 1. If pattern["port"] is the empty string, return error with error code invalid argument.
            // 2. Let port be the result of trying to unescape URL Pattern with pattern["port"].
            if (TRY(get_string(pattern, "port"sv)).is_empty())
                return invalid_argument("URL pattern port must not be empty"sv);
            auto port = TRY(component("port"sv));
            // 3. Append ":" to pattern url.
            pattern_url.append(':');
            // 4. For each codepoint in port:
            //     1. If codepoint is not an ASCII digit:
            //         1. Return error with error code invalid argument.
            for (auto byte : port.bytes()) {
                if (!is_ascii_digit(byte))
                    return invalid_argument("Invalid URL pattern port"sv);
            }
            // 5. Append port to pattern url.
            pattern_url.append(port);
        }
        // 10. Otherwise:
        //      1. Set has port to false.

        // 11. If pattern contains "pathname":
        has_pathname = pattern.has("pathname"sv);
        if (has_pathname) {
            // 1. Let pathname be the result of trying to unescape URL Pattern with pattern["pathname"].
            auto pathname = TRY(component("pathname"sv));
            // 2. If pathname does not start with U+002F (/), then append "/" to pattern url.
            if (!pathname.starts_with('/'))
                pattern_url.append('/');
            // 3. For each codepoint in pathname:
            //     1. If «U+003F (?), U+0023 (#)» contains codepoint:
            //         1. Return error with error code invalid argument.
            if (pathname.contains('?') || pathname.contains('#'))
                return invalid_argument("Invalid URL pattern pathname"sv);
            // 4. Append pathname to pattern url.
            pattern_url.append(pathname);
        }
        // 12. Otherwise:
        //      1. Set has pathname to false.

        // 13. If pattern contains "search":
        has_search = pattern.has("search"sv);
        if (has_search) {
            // 1. Let search be the result of trying to unescape URL pattern with pattern["search"].
            auto search = TRY(component("search"sv));
            // 2. If search does not start with U+003F (?), then append "?" to pattern url.
            if (!search.starts_with('?'))
                pattern_url.append('?');
            // 3. For each codepoint in search:
            //     1. If codepoint is U+0023 (#):
            //         1. Return error with error code invalid argument.
            if (search.contains('#'))
                return invalid_argument("Invalid URL pattern search"sv);
            // 4. Append search to pattern url.
            pattern_url.append(search);
        }
        // 14. Otherwise:
        //      1. Set has search to false.
    }
    // 7. Otherwise, if pattern matches the network.UrlPatternString production:
    else if (type == "string"sv) {
        // 1. Let pattern url be the result of trying to unescape URL pattern with pattern["pattern"].
        auto text = TRY(get_string(pattern, "pattern"sv));
        pattern_url.append(TRY(unescape_url_pattern(text)));
    } else {
        return invalid_argument("Unknown URL pattern type"sv);
    }

    // 8. Let url be the result of parsing pattern url.
    auto url = URL::Parser::basic_parse(pattern_url.string_view());
    // 9. If url is failure, return error with error code invalid argument.
    if (!url.has_value())
        return invalid_argument("Invalid URL pattern"sv);

    // 10. Let parsed be a struct with the following fields:
    UrlPattern parsed;

    // protocol
    //     url's scheme if has protocol is true, or null otherwise.
    if (has_protocol)
        parsed.protocol = MUST(String::from_utf8(url->scheme()));

    // hostname
    //     url's host if has hostname is true, or null otherwise.
    if (has_hostname && url->host().has_value())
        parsed.hostname = MUST(String::from_utf8(url->serialized_host()));

    // port
    //     1. If has port is false:
    //         1. null.
    //     2. Otherwise:
    //         1. If url's scheme is special and url's scheme's default port is not null, and url's port is null or
    //            is equal to scheme's default port:
    //             1. The empty string.
    //         2. Otherwise, if url's port is not null:
    //             1. Serialize an integer with url's port.
    //         3. Otherwise:
    //             1. null.
    // NB: URL parsing already removes a special scheme's default port.
    if (has_port) {
        if (URL::default_port_for_scheme(url->scheme()).has_value() && !url->port().has_value())
            parsed.port = String {};
        else if (auto port = url->port(); port.has_value())
            parsed.port = String::number(*port);
    }

    // pathname
    //     1. If has pathname is false:
    //         1. null.
    //     2. Otherwise:
    //         1. The result of running the URL path serializer with url, if url's path is not the empty string and
    //            is not empty, or null otherwise.
    if (has_pathname) {
        if (auto path = url->serialize_path(); !path.is_empty())
            parsed.pathname = MUST(String::from_utf8(path));
    }

    // search
    //     1. If has search is false:
    //         1. null.
    //     2. Otherwise:
    //         1. The empty string if url's query is null, or url's query otherwise.
    if (has_search)
        parsed.search = MUST(String::from_utf8(url->query().value_or(""sv)));

    // 11. Return success with data parsed.
    return parsed;
}

static ErrorOr<void, Error> validate_protocol_bytes(JsonValue const& value)
{
    if (!value.is_object())
        return invalid_argument("BytesValue must be an object"sv);
    auto const& object = value.as_object();
    auto type = TRY(get_string(object, "type"sv));
    if (!type.is_one_of("string"sv, "base64"sv))
        return invalid_argument("Unknown BytesValue type"sv);
    TRY(get_string(object, "value"sv));
    return {};
}

// https://w3c.github.io/webdriver-bidi/#deserialize-protocol-bytes
static ErrorOr<ByteBuffer, Error> deserialize_protocol_bytes(JsonValue const& value)
{
    TRY(validate_protocol_bytes(value));
    auto const& object = value.as_object();
    auto text = object.get_string("value"sv).release_value();
    // 1. If protocol bytes matches the network.StringValue production, let bytes be UTF-8 encode protocol bytes["value"].
    if (object.get_string("type"sv) == "string"sv)
        return TRY(ByteBuffer::copy(text.bytes()));
    // 2. Otherwise if protocol bytes matches the network.Base64Value production. Let bytes be forgiving-base64 decode protocol bytes["value"].
    auto decoded = decode_base64(text.bytes_as_string_view());
    if (decoded.is_error())
        return invalid_argument("Invalid base64 data"sv);
    // 3. Return bytes.
    return decoded.release_value();
}

static ErrorOr<void, Error> validate_headers(JsonValue const& value)
{
    if (!value.is_array())
        return invalid_argument("Headers must be a list"sv);
    for (auto const& header : value.as_array().values()) {
        if (!header.is_object())
            return invalid_argument("Header must be an object"sv);
        TRY(get_string(header.as_object(), "name"sv));
        auto bytes = header.as_object().get("value"sv);
        if (!bytes.has_value())
            return invalid_argument("Missing header value"sv);
        TRY(validate_protocol_bytes(*bytes));
    }
    return {};
}

// https://w3c.github.io/webdriver-bidi/#create-a-headers-list
ErrorOr<Vector<Header>, Error> create_a_headers_list(JsonValue const& value)
{
    TRY(validate_headers(value));
    // 1. Let headers be an empty header list.
    Vector<Header> headers;
    // 2. For header in protocol headers:
    for (auto const& header : value.as_array().values()) {
        // 1. Let deserialized header be deserialize header with header.
        auto name = header.as_object().get_string("name"sv).release_value();
        auto bytes = TRY(deserialize_protocol_bytes(header.as_object().get("value"sv).release_value()));
        // 2. If deserialized header's name does not match the field-name token production, return error with error code "invalid argument".
        if (!HTTP::is_header_name(name.bytes_as_string_view()))
            return invalid_argument("Invalid header name"sv);
        // 3. If deserialized header's value does not match the header value production, return error with error code "invalid argument".
        if (!HTTP::is_header_value(StringView { bytes.bytes() }))
            return invalid_argument("Invalid header value"sv);
        // 4. Append deserialized header to headers.
        headers.append({ name.to_byte_string(), ByteString { bytes.bytes() } });
    }
    // 3. Return success with data headers
    return headers;
}

static ErrorOr<void, Error> validate_credentials(JsonValue const& value)
{
    if (!value.is_object())
        return invalid_argument("Credentials must be an object"sv);
    auto const& credentials = value.as_object();
    if (TRY(get_string(credentials, "type"sv)) != "password"sv)
        return invalid_argument("Unknown credentials type"sv);
    TRY(get_string(credentials, "username"sv));
    TRY(get_string(credentials, "password"sv));
    return {};
}

static ErrorOr<void, Error> validate_cookies(JsonValue const& value, StringView method)
{
    if (!value.is_array())
        return invalid_argument("Cookies must be a list"sv);
    for (auto const& item : value.as_array().values()) {
        if (!item.is_object())
            return invalid_argument("Cookie must be an object"sv);
        auto const& cookie = item.as_object();
        TRY(get_string(cookie, "name"sv));
        auto bytes = cookie.get("value"sv);
        if (!bytes.has_value())
            return invalid_argument("Missing cookie value"sv);
        TRY(validate_protocol_bytes(*bytes));
        if (method == "network.continueRequest"sv)
            continue;
        for (auto name : { "domain"sv, "expiry"sv, "path"sv }) {
            if (cookie.has(name))
                TRY(get_string(cookie, name));
        }
        for (auto name : { "httpOnly"sv, "secure"sv }) {
            if (auto flag = cookie.get(name); flag.has_value() && !flag->is_bool())
                return invalid_argument("Cookie flag must be a boolean"sv);
        }
        if (cookie.has("sameSite"sv) && !TRY(get_string(cookie, "sameSite"sv)).is_one_of("strict"sv, "lax"sv, "none"sv, "default"sv))
            return invalid_argument("Invalid cookie sameSite"sv);
        if (auto max_age = cookie.get("maxAge"sv); max_age.has_value()) {
            // https://w3c.github.io/webdriver-bidi/#protocol-definition
            // js-int = -9007199254740991..9007199254740991
            if (!max_age->is_number())
                return invalid_argument("Cookie maxAge must be a js-int"sv);
            auto number = max_age->get_double_with_precision_loss().value();
            if (!(number >= -9007199254740991.0 && number <= 9007199254740991.0) || number != static_cast<i64>(number))
                return invalid_argument("Cookie maxAge must be a js-int"sv);
        }
    }
    return {};
}

// https://w3c.github.io/webdriver-bidi/#module-network-commands
ErrorOr<void, Error> validate_interception_parameters(StringView method, JsonObject const& params)
{
    // network.ContinueRequestParameters = {
    //   request: network.Request,
    //   ?body: network.BytesValue,
    //   ?cookies: [*network.CookieHeader],
    //   ?headers: [*network.Header],
    //   ?method: text,
    //   ?url: text,
    // }
    // network.ContinueResponseParameters = {
    //   request: network.Request,
    //   ?cookies: [*network.SetCookieHeader]
    //   ?credentials: network.AuthCredentials,
    //   ?headers: [*network.Header],
    //   ?reasonPhrase: text,
    //   ?statusCode: js-uint,
    // }
    // network.ProvideResponseParameters = {
    //   request: network.Request,
    //   ?body: network.BytesValue,
    //   ?cookies: [*network.SetCookieHeader],
    //   ?headers: [*network.Header],
    //   ?reasonPhrase: text,
    //   ?statusCode: js-uint,
    // }
    // network.FailRequestParameters = {
    //   request: network.Request,
    // }
    // network.ContinueWithAuthParameters = {
    //   request: network.Request,
    //   (network.ContinueWithAuthCredentials // network.ContinueWithAuthNoCredentials)
    // }
    // network.ContinueWithAuthCredentials = (
    //   action: "provideCredentials",
    //   credentials: network.AuthCredentials
    // )
    // network.ContinueWithAuthNoCredentials = (
    //   action: "default" / "cancel"
    // )
    TRY(get_string(params, "request"sv));
    if (method == "network.failRequest"sv)
        return {};
    if (method == "network.continueWithAuth"sv) {
        auto action = TRY(get_string(params, "action"sv));
        if (!action.is_one_of("default"sv, "cancel"sv, "provideCredentials"sv))
            return invalid_argument("Unknown authentication action"sv);
        if (action == "provideCredentials"sv) {
            auto credentials = params.get("credentials"sv);
            if (!credentials.has_value())
                return invalid_argument("Missing authentication credentials"sv);
            TRY(validate_credentials(*credentials));
        }
        return {};
    }
    if (auto headers = params.get("headers"sv); headers.has_value())
        TRY(validate_headers(*headers));
    if (auto cookies = params.get("cookies"sv); cookies.has_value())
        TRY(validate_cookies(*cookies, method));
    if (method.is_one_of("network.continueRequest"sv, "network.provideResponse"sv)) {
        if (auto body = params.get("body"sv); body.has_value())
            TRY(validate_protocol_bytes(*body));
    }
    if (method == "network.continueRequest"sv) {
        for (auto name : { "method"sv, "url"sv }) {
            if (params.has(name))
                TRY(get_string(params, name));
        }
    } else {
        if (params.has("reasonPhrase"sv))
            TRY(get_string(params, "reasonPhrase"sv));
        if (auto status = params.get("statusCode"sv); status.has_value() && !as_js_uint(*status).has_value())
            return invalid_argument("Status code must be a js-uint"sv);
        if (method == "network.continueResponse"sv) {
            if (auto credentials = params.get("credentials"sv); credentials.has_value())
                TRY(validate_credentials(*credentials));
        }
    }
    return {};
}

}
