/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/StringView.h>
#include <LibRegex/Regex.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Methods.h>
#include <LibWeb/Infra/ByteSequences.h>

namespace Web::Fetch::Infrastructure {

// https://fetch.spec.whatwg.org/#concept-method
bool is_method(ReadonlyBytes method)
{
    // A method is a byte sequence that matches the method token production.
    Regex<ECMA262Parser> regex { R"~~~(^[A-Za-z0-9!#$%&'*+\-.^_`|~]+$)~~~" };
    return regex.has_match(StringView { method });
}

// https://fetch.spec.whatwg.org/#cors-safelisted-method
bool is_cors_safelisted_method(ReadonlyBytes method)
{
    // A CORS-safelisted method is a method that is `GET`, `HEAD`, or `POST`.
    return StringView { method }.is_one_of("GET"sv, "HEAD"sv, "POST"sv);
}

// https://fetch.spec.whatwg.org/#forbidden-method
bool is_forbidden_method(ReadonlyBytes method)
{
    // A forbidden method is a method that is a byte-case-insensitive match for `CONNECT`, `TRACE`, or `TRACK`.
    return StringView { method }.is_one_of_ignoring_ascii_case("CONNECT"sv, "TRACE"sv, "TRACK"sv);
}

// https://fetch.spec.whatwg.org/#concept-method-normalize
ByteBuffer normalize_method(ReadonlyBytes method)
{
    // To normalize a method, if it is a byte-case-insensitive match for `DELETE`, `GET`, `HEAD`, `OPTIONS`, `POST`, or `PUT`, byte-uppercase it.
    auto bytes = MUST(ByteBuffer::copy(method));
    if (StringView { method }.is_one_of_ignoring_ascii_case("DELETE"sv, "GET"sv, "HEAD"sv, "OPTIONS"sv, "POST"sv, "PUT"sv))
        Infra::byte_uppercase(bytes);
    return bytes;
}

}
