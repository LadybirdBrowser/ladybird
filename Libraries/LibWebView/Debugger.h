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
