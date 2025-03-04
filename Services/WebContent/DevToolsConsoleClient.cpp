/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericShorthands.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/MemoryStream.h>
#include <LibJS/Print.h>
#include <LibJS/Runtime/BigInt.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <WebContent/ConsoleGlobalEnvironmentExtensions.h>
#include <WebContent/DevToolsConsoleClient.h>
#include <WebContent/PageClient.h>

namespace WebContent {

GC_DEFINE_ALLOCATOR(DevToolsConsoleClient);

GC::Ref<DevToolsConsoleClient> DevToolsConsoleClient::create(JS::Realm& realm, JS::Console& console, PageClient& client)
{
    auto& window = as<Web::HTML::Window>(realm.global_object());
    auto console_global_environment_extensions = realm.create<ConsoleGlobalEnvironmentExtensions>(realm, window);

    return realm.heap().allocate<DevToolsConsoleClient>(realm, console, client, console_global_environment_extensions);
}

DevToolsConsoleClient::DevToolsConsoleClient(JS::Realm& realm, JS::Console& console, PageClient& client, ConsoleGlobalEnvironmentExtensions& console_global_environment_extensions)
    : WebContentConsoleClient(realm, console, client, console_global_environment_extensions)
{
}

DevToolsConsoleClient::~DevToolsConsoleClient() = default;

// https://firefox-source-docs.mozilla.org/devtools/backend/protocol.html#grips
static JsonValue serialize_js_value(JS::Realm& realm, JS::Value value)
{
    auto& vm = realm.vm();

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

    if (value.is_string())
        return value.as_string().utf8_string();

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
        serialized.set("text"sv, MUST(value.as_bigint().big_integer().to_base(10)));
        return serialized;
    }

    if (value.is_symbol())
        return MUST(value.as_symbol().descriptive_string());

    // FIXME: Handle serialization of object grips. For now, we stringify the object.
    if (value.is_object()) {
        Web::HTML::TemporaryExecutionContext execution_context { realm };
        AllocatingMemoryStream stream;

        JS::PrintContext context { vm, stream, true };
        MUST(JS::print(value, context));

        return MUST(String::from_stream(stream, stream.used_buffer_size()));
    }

    return {};
}

void DevToolsConsoleClient::handle_result(JS::Value result)
{
    m_client->did_execute_js_console_input(serialize_js_value(m_realm, result));
}

void DevToolsConsoleClient::report_exception(JS::Error const& exception, bool in_promise)
{
    (void)exception;
    (void)in_promise;
}

void DevToolsConsoleClient::send_messages(i32 start_index)
{
    if (m_console_output.size() - start_index < 1) {
        // When the console is first created, it requests any messages that happened before then, by requesting with
        // start_index=0. If we don't have any messages at all, that is still a valid request, and we can just ignore it.
        if (start_index != 0)
            m_client->console_peer_did_misbehave("Requested non-existent console message index");
        return;
    }

    Vector<WebView::ConsoleOutput> messages { m_console_output.span().slice(start_index) };
    m_client->did_get_unstyled_js_console_messages(start_index, move(messages));
}

// 2.3. Printer(logLevel, args[, options]), https://console.spec.whatwg.org/#printer
JS::ThrowCompletionOr<JS::Value> DevToolsConsoleClient::printer(JS::Console::LogLevel log_level, PrinterArguments arguments)
{
    // FIXME: Implement these.
    if (first_is_one_of(log_level, JS::Console::LogLevel::Table, JS::Console::LogLevel::Trace, JS::Console::LogLevel::Group, JS::Console::LogLevel::GroupCollapsed))
        return JS::js_undefined();

    auto const& argument_values = arguments.get<GC::RootVector<JS::Value>>();

    auto output = TRY(generically_format_values(argument_values));
    m_console->output_debug_message(log_level, output);

    Vector<JsonValue> serialized_arguments;
    serialized_arguments.ensure_capacity(argument_values.size());

    for (auto value : argument_values)
        serialized_arguments.unchecked_append(serialize_js_value(m_console->realm(), value));

    m_console_output.empend(log_level, UnixDateTime::now(), move(serialized_arguments));
    m_client->did_output_js_console_message(m_console_output.size() - 1);

    return JS::js_undefined();
}

}
