/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibHTTP/Method.h>
#include <LibRegex/Regex.h>

namespace HTTP {

// https://fetch.spec.whatwg.org/#concept-method
bool is_method(StringView method)
{
    // A method is a byte sequence that matches the method token production.
    Regex<ECMA262Parser> regex { R"~~~(^[A-Za-z0-9!#$%&'*+\-.^_`|~]+$)~~~" };
    return regex.has_match(method);
}

// https://fetch.spec.whatwg.org/#cors-safelisted-method
bool is_cors_safelisted_method(StringView method)
{
    // A CORS-safelisted method is a method that is `GET`, `HEAD`, or `POST`.
    return method.is_one_of("GET"sv, "HEAD"sv, "POST"sv);
}

// https://fetch.spec.whatwg.org/#forbidden-method
bool is_forbidden_method(StringView method)
{
    // A forbidden method is a method that is a byte-case-insensitive match for `CONNECT`, `TRACE`, or `TRACK`.
    return method.is_one_of_ignoring_ascii_case("CONNECT"sv, "TRACE"sv, "TRACK"sv);
}

// https://fetch.spec.whatwg.org/#concept-method-normalize
ByteString normalize_method(StringView method)
{
    // To normalize a method, if it is a byte-case-insensitive match for `DELETE`, `GET`, `HEAD`, `OPTIONS`, `POST`,
    // or `PUT`, byte-uppercase it.
    static auto NORMALIZED_METHODS = to_array<ByteString>({ "DELETE"sv, "GET"sv, "HEAD"sv, "OPTIONS"sv, "POST"sv, "PUT"sv });

    for (auto const& normalized_method : NORMALIZED_METHODS) {
        if (normalized_method.equals_ignoring_ascii_case(method))
            return normalized_method;
    }

    return method;
}

}
