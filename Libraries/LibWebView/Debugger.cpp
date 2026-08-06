/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWebView/Debugger.h>

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, WebView::DebuggerLocation const& location)
{
    TRY(encoder.encode(location.source));
    TRY(encoder.encode(location.line));
    TRY(encoder.encode(location.column));
    return {};
}

template<>
ErrorOr<WebView::DebuggerLocation> decode(Decoder& decoder)
{
    return WebView::DebuggerLocation {
        .source = TRY(decoder.decode<Web::HTML::ScriptRegistry::Description>()),
        .line = TRY(decoder.decode<u32>()),
        .column = TRY(decoder.decode<u32>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, WebView::DebuggerFrame const& frame)
{
    TRY(encoder.encode(frame.id));
    TRY(encoder.encode(frame.display_name));
    TRY(encoder.encode(frame.location));
    return {};
}

template<>
ErrorOr<WebView::DebuggerFrame> decode(Decoder& decoder)
{
    return WebView::DebuggerFrame {
        .id = TRY(decoder.decode<u64>()),
        .display_name = TRY(decoder.decode<Utf16String>()),
        .location = TRY(decoder.decode<WebView::DebuggerLocation>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, WebView::DebuggerPause const& pause)
{
    TRY(encoder.encode(pause.reason));
    TRY(encoder.encode(pause.frames));
    return {};
}

template<>
ErrorOr<WebView::DebuggerPause> decode(Decoder& decoder)
{
    return WebView::DebuggerPause {
        .reason = TRY(decoder.decode<WebView::DebuggerPauseReason>()),
        .frames = TRY(decoder.decode<Vector<WebView::DebuggerFrame>>()),
    };
}

}
