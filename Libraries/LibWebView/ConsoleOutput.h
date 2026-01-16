/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/JsonValue.h>
#include <AK/String.h>
#include <AK/Time.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibIPC/Forward.h>
#include <LibJS/Console.h>
#include <LibWebView/Forward.h>

namespace WebView {

struct WEBVIEW_API ConsoleLog {
    JS::Console::LogLevel level;
    Vector<JsonValue> arguments;
};

struct WEBVIEW_API StackFrame {
    Optional<String> function;
    Optional<String> file;
    Optional<size_t> line;
    Optional<size_t> column;
};

struct WEBVIEW_API ConsoleError {
    String name;
    String message;
    Vector<StackFrame> trace;
    bool inside_promise { false };
};

struct WEBVIEW_API ConsoleTrace {
    String label;
    Vector<StackFrame> stack;
};

struct WEBVIEW_API ConsoleOutput {
    UnixDateTime timestamp;
    Variant<ConsoleLog, ConsoleError, ConsoleTrace> output;
};

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, WebView::ConsoleLog const&);

template<>
ErrorOr<WebView::ConsoleLog> decode(Decoder&);

template<>
ErrorOr<void> encode(Encoder&, WebView::StackFrame const&);

template<>
ErrorOr<WebView::StackFrame> decode(Decoder&);

template<>
ErrorOr<void> encode(Encoder&, WebView::ConsoleError const&);

template<>
ErrorOr<WebView::ConsoleError> decode(Decoder&);

template<>
ErrorOr<void> encode(Encoder&, WebView::ConsoleTrace const&);

template<>
ErrorOr<WebView::ConsoleTrace> decode(Decoder&);

template<>
WEBVIEW_API ErrorOr<void> encode(Encoder&, WebView::ConsoleOutput const&);

template<>
WEBVIEW_API ErrorOr<WebView::ConsoleOutput> decode(Decoder&);

}
