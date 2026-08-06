/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Span.h>
#include <AK/Types.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibIPC/Forward.h>
#include <LibWeb/HTML/Scripting/ScriptRegistry.h>
#include <LibWebView/Export.h>

namespace WebView {

enum class DebuggerPauseReason : u8 {
    Breakpoint,
    BreakpointConditionThrown,
    DebuggerStatement,
    Entry,
    Exception,
    ResumeLimit,
};

enum class DebuggerResumeMode : u8 {
    Continue,
    StepInto,
    StepOut,
    StepOver,
};

enum class DebuggerBlackboxingOperation : u8 {
    Blackbox,
    Unblackbox,
};

enum class DebuggerValueType : u8 {
    Undefined,
    Null,
    Boolean,
    Number,
    String,
    BigInt,
    Object,
    Uninitialized,
};

struct DebuggerValue {
    DebuggerValueType type { DebuggerValueType::Undefined };
    bool boolean_value { false };
    double number_value { 0 };
    Utf16String text;
    u64 object_id { 0 };
    String object_class;
};

struct DebuggerBinding {
    Utf16String name;
    DebuggerValue value;
    bool writable { false };
};

struct DebuggerProperty {
    Utf16String name;
    Optional<DebuggerValue> value;
    Optional<DebuggerValue> getter;
    Optional<DebuggerValue> setter;
    bool writable { false };
    bool enumerable { false };
    bool configurable { false };
};

struct DebuggerObjectProperties {
    Optional<DebuggerValue> prototype;
    Vector<DebuggerProperty> properties;
};

struct DebuggerEvaluationResult {
    DebuggerValue value;
    bool is_throw { false };
};

enum class DebuggerEnvironmentType : u8 {
    Block,
    Function,
    Object,
};

struct DebuggerEnvironment {
    u64 id { 0 };
    DebuggerEnvironmentType type { DebuggerEnvironmentType::Block };
    Optional<u64> parent_id;
    Optional<Utf16String> function_name;
    Optional<DebuggerValue> object;
    Vector<DebuggerBinding> bindings;
};

struct DebuggerConfiguration {
    bool ignore_caught_exceptions { true };
    bool pause_on_exceptions { false };
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

struct DebuggerBreakpointOptions {
    Optional<Utf16String> condition;
    Optional<Utf16String> log_value;
    bool show_stacktrace { false };
};

struct DebuggerSourcePosition {
    u32 line { 0 };
    u32 column { 0 };

    bool operator==(DebuggerSourcePosition const&) const = default;
};

struct DebuggerBlackboxRange {
    DebuggerSourcePosition start;
    DebuggerSourcePosition end;

    bool operator==(DebuggerBlackboxRange const&) const = default;
};

struct DebuggerBlackboxState {
    void update(ReadonlySpan<DebuggerBlackboxRange> ranges, DebuggerBlackboxingOperation operation)
    {
        if (operation == DebuggerBlackboxingOperation::Blackbox) {
            if (ranges.is_empty()) {
                fully_blackboxed = true;
                blackboxed_ranges.clear();
                unblackboxed_ranges.clear();
                return;
            }

            if (fully_blackboxed) {
                unblackboxed_ranges.remove_all_matching([&](auto const& range) {
                    return ranges.contains_slow(range);
                });
                return;
            }

            for (auto const& range : ranges) {
                if (!blackboxed_ranges.contains_slow(range))
                    blackboxed_ranges.append(range);
            }
            return;
        }

        if (ranges.is_empty()) {
            fully_blackboxed = false;
            blackboxed_ranges.clear();
            unblackboxed_ranges.clear();
            return;
        }

        if (fully_blackboxed) {
            for (auto const& range : ranges) {
                if (!unblackboxed_ranges.contains_slow(range))
                    unblackboxed_ranges.append(range);
            }
            return;
        }

        blackboxed_ranges.remove_all_matching([&](auto const& range) {
            return ranges.contains_slow(range);
        });
    }
    bool is_empty() const { return !fully_blackboxed && blackboxed_ranges.is_empty(); }

    Vector<DebuggerBlackboxRange> blackboxed_ranges;
    Vector<DebuggerBlackboxRange> unblackboxed_ranges;
    bool fully_blackboxed { false };
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
    DebuggerValue this_value;
    Vector<DebuggerValue> arguments;
};

struct DebuggerPause {
    DebuggerPauseReason reason { DebuggerPauseReason::Breakpoint };
    Optional<Utf16String> reason_message;
    Optional<DebuggerValue> exception;
    Vector<DebuggerFrame> frames;
};

}

namespace IPC {

template<>
WEBVIEW_API ErrorOr<void> encode(Encoder&, WebView::DebuggerConfiguration const&);

template<>
WEBVIEW_API ErrorOr<WebView::DebuggerConfiguration> decode(Decoder&);

template<>
WEBVIEW_API ErrorOr<void> encode(Encoder&, WebView::DebuggerValue const&);

template<>
WEBVIEW_API ErrorOr<WebView::DebuggerValue> decode(Decoder&);

template<>
WEBVIEW_API ErrorOr<void> encode(Encoder&, WebView::DebuggerBinding const&);

template<>
WEBVIEW_API ErrorOr<WebView::DebuggerBinding> decode(Decoder&);

template<>
WEBVIEW_API ErrorOr<void> encode(Encoder&, WebView::DebuggerProperty const&);

template<>
WEBVIEW_API ErrorOr<WebView::DebuggerProperty> decode(Decoder&);

template<>
WEBVIEW_API ErrorOr<void> encode(Encoder&, WebView::DebuggerObjectProperties const&);

template<>
WEBVIEW_API ErrorOr<WebView::DebuggerObjectProperties> decode(Decoder&);

template<>
WEBVIEW_API ErrorOr<void> encode(Encoder&, WebView::DebuggerEvaluationResult const&);

template<>
WEBVIEW_API ErrorOr<WebView::DebuggerEvaluationResult> decode(Decoder&);

template<>
WEBVIEW_API ErrorOr<void> encode(Encoder&, WebView::DebuggerEnvironment const&);

template<>
WEBVIEW_API ErrorOr<WebView::DebuggerEnvironment> decode(Decoder&);

template<>
WEBVIEW_API ErrorOr<void> encode(Encoder&, WebView::DebuggerBreakpointLocation const&);

template<>
WEBVIEW_API ErrorOr<WebView::DebuggerBreakpointLocation> decode(Decoder&);

template<>
WEBVIEW_API ErrorOr<void> encode(Encoder&, WebView::DebuggerBreakpointOptions const&);

template<>
WEBVIEW_API ErrorOr<WebView::DebuggerBreakpointOptions> decode(Decoder&);

template<>
WEBVIEW_API ErrorOr<void> encode(Encoder&, WebView::DebuggerSourcePosition const&);

template<>
WEBVIEW_API ErrorOr<WebView::DebuggerSourcePosition> decode(Decoder&);

template<>
WEBVIEW_API ErrorOr<void> encode(Encoder&, WebView::DebuggerBlackboxRange const&);

template<>
WEBVIEW_API ErrorOr<WebView::DebuggerBlackboxRange> decode(Decoder&);

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
