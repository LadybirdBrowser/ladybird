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
#include <LibJS/ConsoleLogLevel.h>
#include <LibWebCommon/Export.h>
#include <LibWebCommon/Forward.h>

namespace WebView {

enum class ConsoleLogType : u8 {
    ConsoleAPI,
    LogPoint,
    LogPointError,
};

struct WEBCOMMON_API StackFrame {
    Optional<String> function;
    Optional<String> file;
    Optional<size_t> line;
    Optional<size_t> column;
};

struct WEBCOMMON_API ConsoleLog {
    JS::ConsoleLogLevel level;
    Vector<JsonValue> arguments;
    ConsoleLogType type { ConsoleLogType::ConsoleAPI };
    Optional<StackFrame> location;
    Optional<Vector<StackFrame>> stacktrace;
};

struct WEBCOMMON_API ConsoleError {
    String name;
    String message;
    Vector<StackFrame> trace;
    bool inside_promise { false };
};

struct WEBCOMMON_API ConsoleTrace {
    String label;
    Vector<StackFrame> stack;
};

struct WEBCOMMON_API ConsoleOutput {
    UnixDateTime timestamp;
    Variant<ConsoleLog, ConsoleError, ConsoleTrace> output;
};

}

namespace IPC {

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, WebView::ConsoleLog const&);

template<>
WEBCOMMON_API ErrorOr<WebView::ConsoleLog> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, WebView::StackFrame const&);

template<>
WEBCOMMON_API ErrorOr<WebView::StackFrame> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, WebView::ConsoleError const&);

template<>
WEBCOMMON_API ErrorOr<WebView::ConsoleError> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, WebView::ConsoleTrace const&);

template<>
WEBCOMMON_API ErrorOr<WebView::ConsoleTrace> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, WebView::ConsoleOutput const&);

template<>
WEBCOMMON_API ErrorOr<WebView::ConsoleOutput> decode(Decoder&);

}
