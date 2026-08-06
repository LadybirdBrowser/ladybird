/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibIPC/Forward.h>
#include <LibWeb/HTML/Scripting/ScriptRegistry.h>
#include <LibWebView/Export.h>

namespace WebView {

enum class DebuggerPauseReason : u8 {
    Breakpoint,
    DebuggerStatement,
    Entry,
};

struct DebuggerConfiguration {
    bool should_pause_on_debugger_statement { true };
    bool skip_breakpoints { false };
};

struct DebuggerBreakpointLocation {
    Optional<Web::HTML::ScriptRegistry::Identifier> source_id;
    Utf16String filename;
    u32 line { 0 };
    Optional<u32> column;

    bool operator==(DebuggerBreakpointLocation const&) const = default;

    bool represents_same_breakpoint_as(DebuggerBreakpointLocation const& other) const
    {
        if (line != other.line || column != other.column)
            return false;
        if (source_id.has_value() && other.source_id.has_value())
            return source_id == other.source_id;
        return filename == other.filename;
    }
};

struct DebuggerSourcePosition {
    u32 line { 0 };
    u32 column { 0 };
};

struct DebuggerLocation {
    Web::HTML::ScriptRegistry::Description source;
    u32 line { 0 };
    u32 column { 0 };
};

struct DebuggerFrame {
    u64 id { 0 };
    Utf16String display_name;
    DebuggerLocation location;
};

struct DebuggerPause {
    DebuggerPauseReason reason { DebuggerPauseReason::Breakpoint };
    Vector<DebuggerFrame> frames;
};

}

namespace IPC {

template<>
WEBVIEW_API ErrorOr<void> encode(Encoder&, WebView::DebuggerConfiguration const&);

template<>
WEBVIEW_API ErrorOr<WebView::DebuggerConfiguration> decode(Decoder&);

template<>
WEBVIEW_API ErrorOr<void> encode(Encoder&, WebView::DebuggerBreakpointLocation const&);

template<>
WEBVIEW_API ErrorOr<WebView::DebuggerBreakpointLocation> decode(Decoder&);

template<>
WEBVIEW_API ErrorOr<void> encode(Encoder&, WebView::DebuggerSourcePosition const&);

template<>
WEBVIEW_API ErrorOr<WebView::DebuggerSourcePosition> decode(Decoder&);

template<>
WEBVIEW_API ErrorOr<void> encode(Encoder&, WebView::DebuggerLocation const&);

template<>
WEBVIEW_API ErrorOr<WebView::DebuggerLocation> decode(Decoder&);

template<>
WEBVIEW_API ErrorOr<void> encode(Encoder&, WebView::DebuggerFrame const&);

template<>
WEBVIEW_API ErrorOr<WebView::DebuggerFrame> decode(Decoder&);

template<>
WEBVIEW_API ErrorOr<void> encode(Encoder&, WebView::DebuggerPause const&);

template<>
WEBVIEW_API ErrorOr<WebView::DebuggerPause> decode(Decoder&);

}
