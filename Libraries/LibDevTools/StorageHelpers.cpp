/*
 * Copyright (c) 2026, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <LibDevTools/StorageHelpers.h>
#include <LibURL/Parser.h>
#include <LibURL/URL.h>

namespace DevTools {

Optional<String> storage_host_for_url(String const& url_string)
{
    auto url = URL::Parser::basic_parse(url_string);
    if (!url.has_value())
        return {};

    auto const& scheme = url->scheme();
    if (scheme == "http"sv || scheme == "https"sv) {
        StringBuilder builder;
        builder.append(scheme);
        builder.append("://"sv);
        builder.append(url->serialized_host());
        if (auto port = url->port(); port.has_value())
            builder.appendff(":{}", *port);
        return builder.to_string_without_validation();
    }

    if (scheme == "about"sv || scheme == "file"sv || scheme == "javascript"sv || scheme == "resource"sv)
        return url->serialize();

    return {};
}

Optional<String> storage_host_name(String const& storage_host)
{
    auto url = URL::Parser::basic_parse(storage_host);
    if (!url.has_value() || !url->host().has_value())
        return {};
    return url->serialized_host();
}

JsonObject to_storage_operation_result(Optional<String> const& error_string)
{
    JsonObject response;
    if (error_string.has_value())
        response.set("errorString"sv, *error_string);
    else
        response.set("errorString"sv, JsonValue {});
    return response;
}

JsonObject to_storage_operation_result(ErrorOr<void> const& result)
{
    JsonObject response;
    if (result.is_error())
        response.set("errorString"sv, result.error().string_literal());
    else
        response.set("errorString"sv, JsonValue {});
    return response;
}

}
