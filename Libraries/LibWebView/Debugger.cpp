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
ErrorOr<void> encode(Encoder& encoder, WebView::DebuggerConfiguration const& configuration)
{
    TRY(encoder.encode(configuration.ignore_caught_exceptions));
    TRY(encoder.encode(configuration.pause_on_exceptions));
    TRY(encoder.encode(configuration.should_pause_on_debugger_statement));
    TRY(encoder.encode(configuration.skip_breakpoints));
    return {};
}

template<>
ErrorOr<WebView::DebuggerConfiguration> decode(Decoder& decoder)
{
    return WebView::DebuggerConfiguration {
        .ignore_caught_exceptions = TRY(decoder.decode<bool>()),
        .pause_on_exceptions = TRY(decoder.decode<bool>()),
        .should_pause_on_debugger_statement = TRY(decoder.decode<bool>()),
        .skip_breakpoints = TRY(decoder.decode<bool>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, WebView::DebuggerValue const& value)
{
    TRY(encoder.encode(value.type));
    TRY(encoder.encode(value.boolean_value));
    TRY(encoder.encode(value.number_value));
    TRY(encoder.encode(value.text));
    TRY(encoder.encode(value.object_id));
    TRY(encoder.encode(value.object_class));
    return {};
}

template<>
ErrorOr<WebView::DebuggerValue> decode(Decoder& decoder)
{
    return WebView::DebuggerValue {
        .type = TRY(decoder.decode<WebView::DebuggerValueType>()),
        .boolean_value = TRY(decoder.decode<bool>()),
        .number_value = TRY(decoder.decode<double>()),
        .text = TRY(decoder.decode<Utf16String>()),
        .object_id = TRY(decoder.decode<u64>()),
        .object_class = TRY(decoder.decode<String>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, WebView::DebuggerBinding const& binding)
{
    TRY(encoder.encode(binding.name));
    TRY(encoder.encode(binding.value));
    TRY(encoder.encode(binding.writable));
    return {};
}

template<>
ErrorOr<WebView::DebuggerBinding> decode(Decoder& decoder)
{
    return WebView::DebuggerBinding {
        .name = TRY(decoder.decode<Utf16String>()),
        .value = TRY(decoder.decode<WebView::DebuggerValue>()),
        .writable = TRY(decoder.decode<bool>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, WebView::DebuggerProperty const& property)
{
    TRY(encoder.encode(property.name));
    TRY(encoder.encode(property.value));
    TRY(encoder.encode(property.getter));
    TRY(encoder.encode(property.setter));
    TRY(encoder.encode(property.writable));
    TRY(encoder.encode(property.enumerable));
    TRY(encoder.encode(property.configurable));
    return {};
}

template<>
ErrorOr<WebView::DebuggerProperty> decode(Decoder& decoder)
{
    return WebView::DebuggerProperty {
        .name = TRY(decoder.decode<Utf16String>()),
        .value = TRY(decoder.decode<Optional<WebView::DebuggerValue>>()),
        .getter = TRY(decoder.decode<Optional<WebView::DebuggerValue>>()),
        .setter = TRY(decoder.decode<Optional<WebView::DebuggerValue>>()),
        .writable = TRY(decoder.decode<bool>()),
        .enumerable = TRY(decoder.decode<bool>()),
        .configurable = TRY(decoder.decode<bool>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, WebView::DebuggerObjectProperties const& properties)
{
    TRY(encoder.encode(properties.prototype));
    TRY(encoder.encode(properties.properties));
    return {};
}

template<>
ErrorOr<WebView::DebuggerObjectProperties> decode(Decoder& decoder)
{
    return WebView::DebuggerObjectProperties {
        .prototype = TRY(decoder.decode<Optional<WebView::DebuggerValue>>()),
        .properties = TRY(decoder.decode<Vector<WebView::DebuggerProperty>>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, WebView::DebuggerEvaluationResult const& result)
{
    TRY(encoder.encode(result.value));
    TRY(encoder.encode(result.is_throw));
    return {};
}

template<>
ErrorOr<WebView::DebuggerEvaluationResult> decode(Decoder& decoder)
{
    return WebView::DebuggerEvaluationResult {
        .value = TRY(decoder.decode<WebView::DebuggerValue>()),
        .is_throw = TRY(decoder.decode<bool>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, WebView::DebuggerEnvironment const& environment)
{
    TRY(encoder.encode(environment.id));
    TRY(encoder.encode(environment.type));
    TRY(encoder.encode(environment.parent_id));
    TRY(encoder.encode(environment.function_name));
    TRY(encoder.encode(environment.object));
    TRY(encoder.encode(environment.bindings));
    return {};
}

template<>
ErrorOr<WebView::DebuggerEnvironment> decode(Decoder& decoder)
{
    return WebView::DebuggerEnvironment {
        .id = TRY(decoder.decode<u64>()),
        .type = TRY(decoder.decode<WebView::DebuggerEnvironmentType>()),
        .parent_id = TRY(decoder.decode<Optional<u64>>()),
        .function_name = TRY(decoder.decode<Optional<Utf16String>>()),
        .object = TRY(decoder.decode<Optional<WebView::DebuggerValue>>()),
        .bindings = TRY(decoder.decode<Vector<WebView::DebuggerBinding>>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, WebView::DebuggerBreakpointLocation const& location)
{
    TRY(encoder.encode(location.source_id));
    TRY(encoder.encode(location.filename));
    TRY(encoder.encode(location.line));
    TRY(encoder.encode(location.column));
    return {};
}

template<>
ErrorOr<WebView::DebuggerBreakpointLocation> decode(Decoder& decoder)
{
    return WebView::DebuggerBreakpointLocation {
        .source_id = TRY(decoder.decode<Optional<Web::HTML::ScriptRegistry::Identifier>>()),
        .filename = TRY(decoder.decode<Utf16String>()),
        .line = TRY(decoder.decode<u32>()),
        .column = TRY(decoder.decode<Optional<u32>>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, WebView::DebuggerBreakpointOptions const& options)
{
    TRY(encoder.encode(options.condition));
    TRY(encoder.encode(options.log_value));
    TRY(encoder.encode(options.show_stacktrace));
    return {};
}

template<>
ErrorOr<WebView::DebuggerBreakpointOptions> decode(Decoder& decoder)
{
    return WebView::DebuggerBreakpointOptions {
        .condition = TRY(decoder.decode<Optional<Utf16String>>()),
        .log_value = TRY(decoder.decode<Optional<Utf16String>>()),
        .show_stacktrace = TRY(decoder.decode<bool>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, WebView::DebuggerSourcePosition const& position)
{
    TRY(encoder.encode(position.line));
    TRY(encoder.encode(position.column));
    return {};
}

template<>
ErrorOr<WebView::DebuggerSourcePosition> decode(Decoder& decoder)
{
    return WebView::DebuggerSourcePosition {
        .line = TRY(decoder.decode<u32>()),
        .column = TRY(decoder.decode<u32>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, WebView::DebuggerBlackboxRange const& range)
{
    TRY(encoder.encode(range.start));
    TRY(encoder.encode(range.end));
    return {};
}

template<>
ErrorOr<WebView::DebuggerBlackboxRange> decode(Decoder& decoder)
{
    return WebView::DebuggerBlackboxRange {
        .start = TRY(decoder.decode<WebView::DebuggerSourcePosition>()),
        .end = TRY(decoder.decode<WebView::DebuggerSourcePosition>()),
    };
}

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
    TRY(encoder.encode(frame.this_value));
    TRY(encoder.encode(frame.arguments));
    return {};
}

template<>
ErrorOr<WebView::DebuggerFrame> decode(Decoder& decoder)
{
    return WebView::DebuggerFrame {
        .id = TRY(decoder.decode<u64>()),
        .display_name = TRY(decoder.decode<Utf16String>()),
        .location = TRY(decoder.decode<WebView::DebuggerLocation>()),
        .this_value = TRY(decoder.decode<WebView::DebuggerValue>()),
        .arguments = TRY(decoder.decode<Vector<WebView::DebuggerValue>>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, WebView::DebuggerPause const& pause)
{
    TRY(encoder.encode(pause.reason));
    TRY(encoder.encode(pause.reason_message));
    TRY(encoder.encode(pause.exception));
    TRY(encoder.encode(pause.frames));
    return {};
}

template<>
ErrorOr<WebView::DebuggerPause> decode(Decoder& decoder)
{
    return WebView::DebuggerPause {
        .reason = TRY(decoder.decode<WebView::DebuggerPauseReason>()),
        .reason_message = TRY(decoder.decode<Optional<Utf16String>>()),
        .exception = TRY(decoder.decode<Optional<WebView::DebuggerValue>>()),
        .frames = TRY(decoder.decode<Vector<WebView::DebuggerFrame>>()),
    };
}

}
