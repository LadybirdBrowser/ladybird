/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWebView/ConsoleOutput.h>

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, WebView::ConsoleOutput const& console_output)
{
    TRY(encoder.encode(console_output.level));
    TRY(encoder.encode(console_output.timestamp));
    TRY(encoder.encode(console_output.arguments));

    return {};
}

template<>
ErrorOr<WebView::ConsoleOutput> IPC::decode(Decoder& decoder)
{
    auto level = TRY(decoder.decode<JS::Console::LogLevel>());
    auto timestamp = TRY(decoder.decode<UnixDateTime>());
    auto arguments = TRY(decoder.decode<Vector<JsonValue>>());

    return WebView::ConsoleOutput { level, timestamp, move(arguments) };
}
