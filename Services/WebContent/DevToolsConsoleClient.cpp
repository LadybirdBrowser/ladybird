/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericShorthands.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/MemoryStream.h>
#include <AK/StringBuilder.h>
#include <AK/Utf16StringBuilder.h>
#include <LibGC/Heap.h>
#include <LibJS/Print.h>
#include <LibJS/Runtime/BigInt.h>
#include <LibJS/Runtime/ErrorData.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/WebDriver/BiDi/BiDi.h>
#include <WebContent/ConsoleGlobalEnvironmentExtensions.h>
#include <WebContent/DevToolsConsoleClient.h>
#include <WebContent/PageClient.h>

namespace WebContent {

GC_DEFINE_ALLOCATOR(DevToolsConsoleClient);

GC::Ref<DevToolsConsoleClient> DevToolsConsoleClient::create(JS::Realm& realm, JS::Console& console, PageClient& client)
{
    auto& window = Web::HTML::relevant_window(realm.global_object());
    auto console_global_environment_extensions = ConsoleGlobalEnvironmentExtensions::create(realm, window);

    return GC::Heap::the().allocate<DevToolsConsoleClient>(console, client, console_global_environment_extensions);
}

DevToolsConsoleClient::DevToolsConsoleClient(JS::Console& console, PageClient& client, ConsoleGlobalEnvironmentExtensions& console_global_environment_extensions)
    : WebContentConsoleClient(console, client, console_global_environment_extensions)
{
}

DevToolsConsoleClient::~DevToolsConsoleClient() = default;

static String console_text(JS::Realm& realm, JS::Value value)
{
    if (value.is_object()) {
        if (auto const* element = Web::Bindings::impl_from<Web::DOM::Element>(&value.as_object())) {
            StringBuilder builder;
            builder.append('<');
            auto name = element->local_name().view();
            builder.append(name.substring_view(0, min(name.length_in_code_units(), 256uz)).to_utf8_but_should_be_ported_to_utf16());
            if (element->id().has_value()) {
                auto id = element->id()->view();
                builder.append(" id=\""sv);
                builder.append(id.substring_view(0, min(id.length_in_code_units(), 256uz)).to_utf8_but_should_be_ported_to_utf16());
                builder.append('"');
            }
            builder.append('>');
            return MUST(builder.to_string());
        }
    }

    Web::HTML::TemporaryExecutionContext execution_context { realm };
    AllocatingMemoryStream stream;
    JS::PrintContext context { .vm = realm.vm(), .stream = &stream, .strip_ansi = true, .compact = true };
    MUST(JS::print(value, context));
    return MUST(String::from_stream(stream, stream.used_buffer_size()));
}

// https://firefox-source-docs.mozilla.org/devtools/backend/protocol.html#grips
JsonValue DevToolsConsoleClient::serialize_value(JS::Realm& realm, JS::Value value)
{
    auto serialize_type = [](StringView type) {
        JsonObject serialized;
        serialized.set("type"sv, type);
        return serialized;
    };

    if (value.is_undefined())
        return serialize_type("undefined"sv);

    if (value.is_null())
        return serialize_type("null"sv);

    if (value.is_boolean())
        return value.as_bool();

    if (value.is_string()) {
        auto text = value.as_string().utf16_string_view();
        auto prefix = text.substring_view(0, min(text.length_in_code_units(), 1024uz)).to_utf8_but_should_be_ported_to_utf16();
        if (text.length_in_code_units() > 1024)
            return MUST(String::formatted("{}...", prefix));
        return prefix;
    }

    if (value.is_number()) {
        if (value.is_nan())
            return serialize_type("NaN"sv);
        if (value.is_positive_infinity())
            return serialize_type("Infinity"sv);
        if (value.is_negative_infinity())
            return serialize_type("-Infinity"sv);
        if (value.is_negative_zero())
            return serialize_type("-0"sv);
        return value.as_double();
    }

    if (value.is_bigint()) {
        auto serialized = serialize_type("BigInt"sv);
        auto text = console_text(realm, value);
        serialized.set("text"sv, text.ends_with('n') ? MUST(text.substring_from_byte_offset(0, text.bytes_as_string_view().length() - 1)) : text);
        return serialized;
    }

    if (value.is_symbol())
        return console_text(realm, value);

    if (value.is_object())
        return console_text(realm, value);

    return {};
}

void DevToolsConsoleClient::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    m_object_table.visit_edges(visitor);
    m_logged_objects.visit_edges(visitor);
}

void DevToolsConsoleClient::handle_result(JS::Value result)
{
    auto& settings = Web::HTML::relevant_settings_object(m_console_global_environment_extensions->binding_object());
    Web::HTML::TemporaryExecutionContext execution_context { settings.realm() };
    m_client->did_execute_js_console_input(m_object_table.serialize_value(result));
}

bool DevToolsConsoleClient::has_object(u64 object_id) const
{
    return m_object_table.contains(object_id) || m_logged_objects.contains(object_id);
}

ErrorOr<WebView::DebuggerObjectProperties> DevToolsConsoleClient::properties_for_object(u64 object_id)
{
    auto& settings = Web::HTML::relevant_settings_object(m_console_global_environment_extensions->binding_object());
    Web::HTML::TemporaryExecutionContext execution_context { settings.realm() };
    if (m_logged_objects.contains(object_id))
        return m_logged_objects.properties_for_object(object_id);
    return m_object_table.properties_for_object(object_id);
}

void DevToolsConsoleClient::report_exception(Utf16View name, Utf16View message, JS::ErrorData const& error_data, bool in_promise)
{
    Vector<WebView::StackFrame> trace;
    trace.ensure_capacity(error_data.traceback().size());

    for (auto const& frame : error_data.traceback()) {
        auto const& source_range = frame.source_range();
        WebView::StackFrame stack_frame;

        if (!frame.function_name.is_empty())
            stack_frame.function = frame.function_name.to_utf8();

        if (!source_range.filename().is_empty() || source_range.start.line != 0 || source_range.start.column != 0) {
            stack_frame.file = source_range.filename().to_utf8();
            stack_frame.line = source_range.start.line;
            stack_frame.column = source_range.start.column;
        }

        if (stack_frame.function.has_value() || stack_frame.file.has_value())
            trace.unchecked_append(move(stack_frame));
    }

    send_console_output({
        .timestamp = UnixDateTime::now(),
        .output = WebView::ConsoleError {
            .name = MUST(name.to_utf8()),
            .message = MUST(message.to_utf8()),
            .trace = move(trace),
            .inside_promise = in_promise,
        },
    });
}

void DevToolsConsoleClient::end_group()
{
    send_console_output({ .timestamp = UnixDateTime::now(), .output = WebView::ConsoleGroupEnd {} });
}

void DevToolsConsoleClient::send_console_output(WebView::ConsoleOutput console_output)
{
    m_client->buffer_console_output(console_output);

    auto is_error = console_output.output.has<WebView::ConsoleError>();
    if (is_error ? m_client->devtools_takes_console_errors() : m_client->devtools_takes_console_messages())
        m_client->did_output_js_console_message(move(console_output));
}

// 2.3. Printer(logLevel, args[, options]), https://console.spec.whatwg.org/#printer
JS::ThrowCompletionOr<JS::Value> DevToolsConsoleClient::printer(JS::Console::LogLevel log_level, PrinterArguments arguments)
{
    // https://w3c.github.io/webdriver-bidi/#event-log-entryAdded
    if (Web::WebDriver::BiDi::is_active())
        Web::WebDriver::BiDi::console_steps(Web::HTML::principal_realm_settings_object(m_console->realm()), log_level, arguments);

    if (log_level == JS::Console::LogLevel::Trace) {
        auto const& trace = arguments.get<JS::Console::Trace>();

        m_console->output_debug_message(log_level, trace.label);

        Vector<WebView::StackFrame> stack_frames;
        stack_frames.ensure_capacity(trace.stack.size());

        for (auto const& frame : trace.stack) {
            Optional<String> source_file;
            if (frame.source_file.has_value())
                source_file = frame.source_file->to_utf8();

            stack_frames.unchecked_append(WebView::StackFrame {
                .function = frame.function_name.to_utf8(),
                .file = move(source_file),
                .line = frame.line,
                .column = frame.column,
            });
        }

        send_console_output({
            .timestamp = UnixDateTime::now(),
            .output = WebView::ConsoleTrace {
                .label = trace.label.to_utf8(),
                .stack = move(stack_frames),
            },
        });

        return JS::js_undefined();
    }

    if (first_is_one_of(log_level, JS::Console::LogLevel::Group, JS::Console::LogLevel::GroupCollapsed)) {
        auto const& group = arguments.get<JS::Console::Group>();
        m_console->output_debug_message(log_level, group.label);

        send_console_output({
            .timestamp = UnixDateTime::now(),
            .output = WebView::ConsoleLog {
                .level = log_level,
                .arguments = { group.label.to_utf8() },
                .type = WebView::ConsoleLogType::ConsoleAPI,
                .location = {},
                .stacktrace = {},
            },
        });
        return JS::js_undefined();
    }

    auto const& argument_values = arguments.get<GC::RootVector<JS::Value>>();

    Vector<JsonValue> buffered_arguments;
    Utf16StringBuilder output_builder;
    auto argument_count = min(argument_values.size(), 100uz);
    for (size_t index = 0; index < argument_count; ++index) {
        if (output_builder.length_in_code_units() >= 8192) {
            output_builder.append_ascii(" ..."sv);
            buffered_arguments.append("..."_string);
            break;
        }
        auto value = argument_values[index];
        auto serialized = serialize_value(m_console->realm(), value);
        auto text = value.is_object() ? serialized.as_string() : console_text(m_console->realm(), value);
        if (index > 0)
            output_builder.append_ascii(' ');
        auto utf16_text = Utf16String::from_utf8(text);
        auto remaining = 8192 - output_builder.length_in_code_units();
        output_builder.append(utf16_text.utf16_view().substring_view(0, min(utf16_text.length_in_code_units(), remaining)));
        if (value.is_object() && !Web::Bindings::impl_from<Web::DOM::Element>(&value.as_object()))
            buffered_arguments.append(MUST(String::from_utf8(value.as_object().class_name())));
        else
            buffered_arguments.append(move(serialized));
        if (utf16_text.length_in_code_units() > remaining || (index + 1 == argument_count && argument_count < argument_values.size())) {
            output_builder.append_ascii(" ..."sv);
            buffered_arguments.append("..."_string);
            break;
        }
    }
    auto output = output_builder.to_string();
    m_console->output_debug_message(log_level, output);

    auto timestamp = UnixDateTime::now();
    auto console_log = [&](JS::Console::LogLevel level, Vector<JsonValue> serialized_arguments) {
        return WebView::ConsoleOutput {
            .timestamp = timestamp,
            .output = WebView::ConsoleLog {
                .level = level,
                .arguments = move(serialized_arguments),
                .type = WebView::ConsoleLogType::ConsoleAPI,
                .location = {},
                .stacktrace = {},
            },
        };
    };

    // The console hands a table over as the one object that holds its rows and columns.
    auto is_table = log_level == JS::Console::LogLevel::Table && !argument_values.is_empty() && argument_values.first().is_object();

    // The history keeps primitive values and object summaries, without retaining objects or their properties.
    m_client->buffer_console_output(console_log(is_table ? JS::Console::LogLevel::Log : log_level, buffered_arguments));

    // A DevTools client that takes the messages gets grips for the objects instead, which it may expand.
    if (!m_client->devtools_takes_console_messages())
        return JS::js_undefined();

    Vector<JsonValue> serialized_arguments;
    if (is_table) {
        auto table_data = argument_values.first();
        serialized_arguments = m_logged_objects.serialize_console_table_arguments(m_console->realm().vm(), table_data.as_object());
    } else {
        serialized_arguments = m_logged_objects.serialize_message_arguments(argument_values.span());
    }
    m_client->did_output_js_console_message(console_log(log_level, move(serialized_arguments)));

    return JS::js_undefined();
}

}
