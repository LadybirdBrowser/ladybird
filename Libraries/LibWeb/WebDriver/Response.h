/*
 * Copyright (c) 2022, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/JsonValue.h>
#include <AK/Variant.h>
#include <LibIPC/Forward.h>
#include <LibWeb/WebDriver/Error.h>

namespace Web::WebDriver {

// FIXME: Ideally, this could be `using Response = ErrorOr<JsonValue, Error>`, but that won't be
//        default-constructible, which is a requirement for the generated IPC.
struct [[nodiscard]] Response {
    Response() = default;
    Response(JsonValue&&);
    Response(Error&&);

    JsonValue& value() { return m_value_or_error.template get<JsonValue>(); }
    JsonValue const& value() const { return m_value_or_error.template get<JsonValue>(); }

    Error& error() { return m_value_or_error.template get<Error>(); }
    Error const& error() const { return m_value_or_error.template get<Error>(); }

    bool is_error() const { return m_value_or_error.template has<Error>(); }

    JsonValue release_value() { return move(value()); }
    Error release_error() { return move(error()); }

    template<typename... Visitors>
    decltype(auto) visit(Visitors&&... visitors) const
    {
        return m_value_or_error.visit(forward<Visitors>(visitors)...);
    }

private:
    // Note: Empty is only a possible state until the Response has been decoded by IPC.
    Variant<Empty, JsonValue, Error> m_value_or_error;
};

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, Web::WebDriver::Response const&);

template<>
ErrorOr<Web::WebDriver::Response> decode(Decoder&);

}
