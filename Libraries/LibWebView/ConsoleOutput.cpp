/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWebView/ConsoleOutput.h>

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, WebView::ConsoleLog const& log)
{
    TRY(encoder.encode(log.level));
    TRY(encoder.encode(log.arguments));

    return {};
}

template<>
ErrorOr<WebView::ConsoleLog> IPC::decode(Decoder& decoder)
{
    auto level = TRY(decoder.decode<JS::Console::LogLevel>());
    auto arguments = TRY(decoder.decode<Vector<JsonValue>>());

    return WebView::ConsoleLog { level, move(arguments) };
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, WebView::StackFrame const& frame)
{
    TRY(encoder.encode(frame.function));
    TRY(encoder.encode(frame.file));
    TRY(encoder.encode(frame.line));
    TRY(encoder.encode(frame.column));

    return {};
}

template<>
ErrorOr<WebView::StackFrame> IPC::decode(Decoder& decoder)
{
    auto function = TRY(decoder.decode<Optional<String>>());
    auto file = TRY(decoder.decode<Optional<String>>());
    auto line = TRY(decoder.decode<Optional<size_t>>());
    auto column = TRY(decoder.decode<Optional<size_t>>());

    return WebView::StackFrame { move(function), move(file), line, column };
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, WebView::ConsoleError const& error)
{
    TRY(encoder.encode(error.name));
    TRY(encoder.encode(error.message));
    TRY(encoder.encode(error.trace));
    TRY(encoder.encode(error.inside_promise));

    return {};
}

template<>
ErrorOr<WebView::ConsoleError> IPC::decode(Decoder& decoder)
{
    auto name = TRY(decoder.decode<String>());
    auto message = TRY(decoder.decode<String>());
    auto trace = TRY(decoder.decode<Vector<WebView::StackFrame>>());
    auto inside_promise = TRY(decoder.decode<bool>());

    return WebView::ConsoleError { move(name), move(message), move(trace), inside_promise };
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, WebView::ConsoleOutput const& output)
{
    TRY(encoder.encode(output.timestamp));
    TRY(encoder.encode(output.output));

    return {};
}

template<>
ErrorOr<WebView::ConsoleOutput> IPC::decode(Decoder& decoder)
{
    auto timestamp = TRY(decoder.decode<UnixDateTime>());
    auto output = TRY(decoder.decode<Variant<WebView::ConsoleLog, WebView::ConsoleError>>());

    return WebView::ConsoleOutput { timestamp, move(output) };
}
