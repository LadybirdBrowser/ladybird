/*
 * Copyright (c) 2021, Brandon Scott <xeon.productions@gmail.com>
 * Copyright (c) 2020, Hunter Salyer <thefalsehonesty@gmail.com>
 * Copyright (c) 2021-2022, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2024, Gasim Gasimzada <gasim@gasimzada.net>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/MemoryStream.h>
#include <AK/String.h>
#include <LibJS/MarkupGenerator.h>
#include <LibJS/Print.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/HTML/Window.h>
#include <WebContent/ConsoleGlobalEnvironmentExtensions.h>
#include <WebContent/InspectorConsoleClient.h>
#include <WebContent/PageClient.h>

namespace WebContent {

GC_DEFINE_ALLOCATOR(InspectorConsoleClient);

GC::Ref<InspectorConsoleClient> InspectorConsoleClient::create(JS::Realm& realm, JS::Console& console, PageClient& client)
{
    auto& window = as<Web::HTML::Window>(realm.global_object());
    auto console_global_environment_extensions = realm.create<ConsoleGlobalEnvironmentExtensions>(realm, window);

    return realm.heap().allocate<InspectorConsoleClient>(realm, console, client, console_global_environment_extensions);
}

InspectorConsoleClient::InspectorConsoleClient(JS::Realm& realm, JS::Console& console, PageClient& client, ConsoleGlobalEnvironmentExtensions& console_global_environment_extensions)
    : WebContentConsoleClient(realm, console, client, console_global_environment_extensions)
{
}

InspectorConsoleClient::~InspectorConsoleClient() = default;

void InspectorConsoleClient::handle_result(JS::Value result)
{
    print_html(JS::MarkupGenerator::html_from_value(result).release_value_but_fixme_should_propagate_errors());
}

void InspectorConsoleClient::report_exception(JS::Error const& exception, bool in_promise)
{
    print_html(JS::MarkupGenerator::html_from_error(exception, in_promise).release_value_but_fixme_should_propagate_errors());
}

void InspectorConsoleClient::send_messages(i32 start_index)
{
    // FIXME: Cap the number of messages we send at once?
    auto messages_to_send = m_message_log.size() - start_index;
    if (messages_to_send < 1) {
        // When the console is first created, it requests any messages that happened before
        // then, by requesting with start_index=0. If we don't have any messages at all, that
        // is still a valid request, and we can just ignore it.
        if (start_index != 0)
            m_client->console_peer_did_misbehave("Requested non-existent console message index.");
        return;
    }

    // FIXME: Replace with a single Vector of message structs
    Vector<String> message_types;
    Vector<String> messages;
    message_types.ensure_capacity(messages_to_send);
    messages.ensure_capacity(messages_to_send);

    for (size_t i = start_index; i < m_message_log.size(); i++) {
        auto& message = m_message_log[i];
        switch (message.type) {
        case ConsoleOutput::Type::HTML:
            message_types.append("html"_string);
            break;
        case ConsoleOutput::Type::Clear:
            message_types.append("clear"_string);
            break;
        case ConsoleOutput::Type::BeginGroup:
            message_types.append("group"_string);
            break;
        case ConsoleOutput::Type::BeginGroupCollapsed:
            message_types.append("groupCollapsed"_string);
            break;
        case ConsoleOutput::Type::EndGroup:
            message_types.append("groupEnd"_string);
            break;
        }

        messages.append(message.data);
    }

    m_client->did_get_js_console_messages(start_index, move(message_types), move(messages));
}

// 2.3. Printer(logLevel, args[, options]), https://console.spec.whatwg.org/#printer
JS::ThrowCompletionOr<JS::Value> InspectorConsoleClient::printer(JS::Console::LogLevel log_level, PrinterArguments arguments)
{
    auto styling = escape_html_entities(m_current_message_style.string_view());
    m_current_message_style.clear();

    if (log_level == JS::Console::LogLevel::Table) {
        auto& vm = m_console->realm().vm();

        auto table_args = arguments.get<GC::RootVector<JS::Value>>();
        auto& table = table_args.at(0).as_object();
        auto& columns = TRY(table.get(vm.names.columns)).as_array().indexed_properties();
        auto& rows = TRY(table.get(vm.names.rows)).as_array().indexed_properties();

        StringBuilder html;

        html.appendff("<div class=\"console-log-table\">");
        html.appendff("<table>");
        html.appendff("<thead>");
        html.appendff("<tr>");
        for (auto const& col : columns) {
            auto index = col.index();
            auto value = columns.storage()->get(index).value().value;
            html.appendff("<td>{}</td>", value);
        }

        html.appendff("</tr>");
        html.appendff("</thead>");
        html.appendff("<tbody>");

        for (auto const& row : rows) {
            auto row_index = row.index();
            auto& row_obj = rows.storage()->get(row_index).value().value.as_object();
            html.appendff("<tr>");

            for (auto const& col : columns) {
                auto col_index = col.index();
                auto col_name = columns.storage()->get(col_index).value().value;

                auto property_key = TRY(JS::PropertyKey::from_value(vm, col_name));
                auto cell = TRY(row_obj.get(property_key));
                html.appendff("<td>");
                if (TRY(cell.is_array(vm))) {
                    AllocatingMemoryStream stream;
                    JS::PrintContext ctx { vm, stream, true };
                    TRY_OR_THROW_OOM(vm, stream.write_until_depleted(" "sv.bytes()));
                    TRY_OR_THROW_OOM(vm, JS::print(cell, ctx));
                    auto output = TRY_OR_THROW_OOM(vm, String::from_stream(stream, stream.used_buffer_size()));

                    auto size = cell.as_array().indexed_properties().array_like_size();
                    html.appendff("<details><summary>Array({})</summary>{}</details>", size, output);

                } else if (cell.is_object()) {
                    AllocatingMemoryStream stream;
                    JS::PrintContext ctx { vm, stream, true };
                    TRY_OR_THROW_OOM(vm, stream.write_until_depleted(" "sv.bytes()));
                    TRY_OR_THROW_OOM(vm, JS::print(cell, ctx));
                    auto output = TRY_OR_THROW_OOM(vm, String::from_stream(stream, stream.used_buffer_size()));

                    html.appendff("<details><summary>Object({{...}})</summary>{}</details>", output);
                } else if (cell.is_function() || cell.is_constructor()) {
                    html.appendff("ƒ");
                } else if (!cell.is_undefined()) {
                    html.appendff("{}", cell);
                }
                html.appendff("</td>");
            }

            html.appendff("</tr>");
        }

        html.appendff("</tbody>");
        html.appendff("</table>");
        html.appendff("</div>");
        print_html(MUST(html.to_string()));

        auto output = TRY(generically_format_values(table_args));
        m_console->output_debug_message(log_level, output);

        return JS::js_undefined();
    }

    if (log_level == JS::Console::LogLevel::Trace) {
        auto trace = arguments.get<JS::Console::Trace>();
        StringBuilder html;
        if (!trace.label.is_empty())
            html.appendff("<span class='title' style='{}'>{}</span><br>", styling, escape_html_entities(trace.label));

        html.append("<span class='trace'>"sv);
        for (auto& function_name : trace.stack)
            html.appendff("-> {}<br>", escape_html_entities(function_name));
        html.append("</span>"sv);

        print_html(MUST(html.to_string()));
        return JS::js_undefined();
    }

    if (log_level == JS::Console::LogLevel::Group || log_level == JS::Console::LogLevel::GroupCollapsed) {
        auto group = arguments.get<JS::Console::Group>();
        begin_group(MUST(String::formatted("<span style='{}'>{}</span>", styling, escape_html_entities(group.label))), log_level == JS::Console::LogLevel::Group);
        return JS::js_undefined();
    }

    auto output = TRY(generically_format_values(arguments.get<GC::RootVector<JS::Value>>()));
    m_console->output_debug_message(log_level, output);

    StringBuilder html;
    switch (log_level) {
    case JS::Console::LogLevel::Debug:
        html.appendff("<span class=\"debug\" style=\"{}\">(d) "sv, styling);
        break;
    case JS::Console::LogLevel::Error:
        html.appendff("<span class=\"error\" style=\"{}\">(e) "sv, styling);
        break;
    case JS::Console::LogLevel::Info:
        html.appendff("<span class=\"info\" style=\"{}\">(i) "sv, styling);
        break;
    case JS::Console::LogLevel::Log:
        html.appendff("<span class=\"log\" style=\"{}\"> "sv, styling);
        break;
    case JS::Console::LogLevel::Warn:
    case JS::Console::LogLevel::CountReset:
        html.appendff("<span class=\"warn\" style=\"{}\">(w) "sv, styling);
        break;
    default:
        html.appendff("<span style=\"{}\">"sv, styling);
        break;
    }

    html.append(escape_html_entities(output));
    html.append("</span>"sv);
    print_html(MUST(html.to_string()));

    return JS::js_undefined();
}

}
