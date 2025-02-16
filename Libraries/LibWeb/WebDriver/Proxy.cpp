/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonValue.h>
#include <LibURL/Parser.h>
#include <LibURL/URL.h>
#include <LibWeb/WebDriver/Proxy.h>

namespace Web::WebDriver {

// https://w3c.github.io/webdriver/#dfn-has-proxy-configuration
// An endpoint node has an associated has proxy configuration flag that indicates whether the proxy is already configured.
// The default value of the flag is true if the endpoint doesn't support proxy configuration, or false otherwise.
static constexpr bool s_default_has_proxy_configuration = true;
static bool s_has_proxy_configuration = s_default_has_proxy_configuration;

bool has_proxy_configuration()
{
    return s_has_proxy_configuration;
}

void set_has_proxy_configuration(bool has_proxy_configuration)
{
    s_has_proxy_configuration = has_proxy_configuration;
}

void reset_has_proxy_configuration()
{
    s_has_proxy_configuration = s_default_has_proxy_configuration;
}

// https://w3c.github.io/webdriver/#dfn-proxy-configuration
static ErrorOr<void, Error> validate_proxy_item(StringView key, JsonValue const& value)
{
    if (key == "proxyType"sv) {
        if (!value.is_string())
            return Error::from_code(ErrorCode::InvalidArgument, "Proxy configuration item 'proxyType' must be a string"sv);
        if (!value.as_string().is_one_of("pac"sv, "direct"sv, "autodetect"sv, "system"sv, "manual"sv))
            return Error::from_code(ErrorCode::InvalidArgument, "Invalid 'proxyType' value"sv);
        return {};
    }

    if (key == "proxyAutoconfigUrl"sv) {
        if (!value.is_string())
            return Error::from_code(ErrorCode::InvalidArgument, "Proxy configuration item 'proxyAutoconfigUrl' must be a string"sv);
        if (auto url = URL::Parser::basic_parse(value.as_string()); !url.has_value())
            return Error::from_code(ErrorCode::InvalidArgument, "Proxy configuration item 'proxyAutoconfigUrl' must be a valid URL"sv);
        return {};
    }

    if (key == "ftpProxy"sv) {
        if (!value.is_string())
            return Error::from_code(ErrorCode::InvalidArgument, "Proxy configuration item 'ftpProxy' must be a string"sv);
        if (auto url = URL::Parser::basic_parse(value.as_string()); !url.has_value() || url->scheme() != "ftp"sv)
            return Error::from_code(ErrorCode::InvalidArgument, "Proxy configuration item 'ftpProxy' must be a valid FTP URL"sv);
        return {};
    }

    if (key == "httpProxy"sv) {
        if (!value.is_string())
            return Error::from_code(ErrorCode::InvalidArgument, "Proxy configuration item 'httpProxy' must be a string"sv);
        if (auto url = URL::Parser::basic_parse(value.as_string()); !url.has_value() || url->scheme() != "http"sv)
            return Error::from_code(ErrorCode::InvalidArgument, "Proxy configuration item 'httpProxy' must be a valid HTTP URL"sv);
        return {};
    }

    if (key == "noProxy"sv) {
        if (!value.is_array())
            return Error::from_code(ErrorCode::InvalidArgument, "Proxy configuration item 'noProxy' must be a list"sv);

        TRY(value.as_array().try_for_each([&](JsonValue const& item) -> ErrorOr<void, Error> {
            if (!item.is_string())
                return Error::from_code(ErrorCode::InvalidArgument, "Proxy configuration item 'noProxy' must be a list of strings"sv);
            return {};
        }));

        return {};
    }

    if (key == "sslProxy"sv) {
        if (!value.is_string())
            return Error::from_code(ErrorCode::InvalidArgument, "Proxy configuration item 'sslProxy' must be a string"sv);
        if (auto url = URL::Parser::basic_parse(value.as_string()); !url.has_value() || url->scheme() != "https"sv)
            return Error::from_code(ErrorCode::InvalidArgument, "Proxy configuration item 'sslProxy' must be a valid HTTPS URL"sv);
        return {};
    }

    if (key == "socksProxy"sv) {
        if (!value.is_string())
            return Error::from_code(ErrorCode::InvalidArgument, "Proxy configuration item 'proxyAutoconfigUrl' must be a string"sv);
        if (auto url = URL::Parser::basic_parse(value.as_string()); !url.has_value())
            return Error::from_code(ErrorCode::InvalidArgument, "Proxy configuration item 'proxyAutoconfigUrl' must be a valid URL"sv);
        return {};
    }

    if (key == "socksVersion"sv) {
        if (!value.is_integer<u8>())
            return Error::from_code(ErrorCode::InvalidArgument, "Proxy configuration item 'socksVersion' must be an integer in the range [0, 255]"sv);
        return {};
    }

    return Error::from_code(ErrorCode::InvalidArgument, "Invalid proxy configuration item"sv);
}

// https://w3c.github.io/webdriver/#dfn-deserialize-as-a-proxy
ErrorOr<JsonObject, Error> deserialize_as_a_proxy(JsonValue const& parameter)
{
    // 1. If parameter is not a JSON Object return an error with error code invalid argument.
    if (!parameter.is_object())
        return Error::from_code(ErrorCode::InvalidArgument, "Capability proxy must be an object"sv);

    // 2. Let proxy be a new, empty proxy configuration object.
    JsonObject proxy;

    // 3. For each enumerable own property in parameter run the following substeps:
    TRY(parameter.as_object().try_for_each_member([&](ByteString const& key, JsonValue const& value) -> ErrorOr<void, Error> {
        // 1. Let key be the name of the property.
        // 2. Let value be the result of getting a property named name from capability.

        // 3. If there is no matching key for key in the proxy configuration table return an error with error code invalid argument.
        // 4. If value is not one of the valid values for that key, return an error with error code invalid argument.
        TRY(validate_proxy_item(key, value));

        // 5. Set a property key to value on proxy.
        proxy.set(key, value);

        return {};
    }));

    return proxy;
}

}
