/*
 * Copyright (c) 2022, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWeb/WebDriver/Response.h>

enum class ResponseType : u8 {
    Success,
    Error,
};

namespace Web::WebDriver {

Response::Response(JsonValue&& value)
    : m_value_or_error(move(value))
{
}

Response::Response(Error&& error)
    : m_value_or_error(move(error))
{
}

}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::WebDriver::Response const& response)
{
    return response.visit(
        [](Empty) -> ErrorOr<void> { VERIFY_NOT_REACHED(); },
        [&](JsonValue const& value) -> ErrorOr<void> {
            TRY(encoder.encode(ResponseType::Success));
            TRY(encoder.encode(value));
            return {};
        },
        [&](Web::WebDriver::Error const& error) -> ErrorOr<void> {
            TRY(encoder.encode(ResponseType::Error));
            TRY(encoder.encode(error.http_status));
            TRY(encoder.encode(error.error));
            TRY(encoder.encode(error.message));
            TRY(encoder.encode(error.data));
            return {};
        });
}

template<>
ErrorOr<Web::WebDriver::Response> IPC::decode(Decoder& decoder)
{
    auto type = TRY(decoder.decode<ResponseType>());

    switch (type) {
    case ResponseType::Success:
        return TRY(decoder.decode<JsonValue>());

    case ResponseType::Error: {
        auto http_status = TRY(decoder.decode<unsigned>());
        auto error = TRY(decoder.decode<ByteString>());
        auto message = TRY(decoder.decode<ByteString>());
        auto data = TRY(decoder.decode<Optional<JsonValue>>());

        return Web::WebDriver::Error { http_status, move(error), move(message), move(data) };
    }
    }

    VERIFY_NOT_REACHED();
}
