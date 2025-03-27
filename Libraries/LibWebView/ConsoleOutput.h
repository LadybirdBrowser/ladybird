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

namespace WebView {

struct ConsoleLog {
    JS::Console::LogLevel level;
    Vector<JsonValue> arguments;
};

struct StackFrame {
    Optional<String> function;
    Optional<String> file;
    Optional<size_t> line;
    Optional<size_t> column;
};

struct ConsoleError {
    String name;
    String message;
    Vector<StackFrame> trace;
    bool inside_promise { false };
};

struct ConsoleOutput {
    UnixDateTime timestamp;
    Variant<ConsoleLog, ConsoleError> output;
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
ErrorOr<void> encode(Encoder&, WebView::ConsoleOutput const&);

template<>
ErrorOr<WebView::ConsoleOutput> decode(Decoder&);

}
