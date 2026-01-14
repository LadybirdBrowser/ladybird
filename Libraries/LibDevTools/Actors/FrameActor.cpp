/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Enumerate.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <LibDevTools/Actors/AccessibilityActor.h>
#include <LibDevTools/Actors/CSSPropertiesActor.h>
#include <LibDevTools/Actors/ConsoleActor.h>
#include <LibDevTools/Actors/FrameActor.h>
#include <LibDevTools/Actors/InspectorActor.h>
#include <LibDevTools/Actors/NetworkEventActor.h>
#include <LibDevTools/Actors/StyleSheetsActor.h>
#include <LibDevTools/Actors/TabActor.h>
#include <LibDevTools/Actors/ThreadActor.h>
#include <LibDevTools/DevToolsDelegate.h>
#include <LibDevTools/DevToolsServer.h>
#include <LibWebView/ConsoleOutput.h>

namespace DevTools {

NonnullRefPtr<FrameActor> FrameActor::create(DevToolsServer& devtools, String name, WeakPtr<TabActor> tab, WeakPtr<CSSPropertiesActor> css_properties, WeakPtr<ConsoleActor> console, WeakPtr<InspectorActor> inspector, WeakPtr<StyleSheetsActor> style_sheets, WeakPtr<ThreadActor> thread, WeakPtr<AccessibilityActor> accessibility)
{
    return adopt_ref(*new FrameActor(devtools, move(name), move(tab), move(css_properties), move(console), move(inspector), move(style_sheets), move(thread), move(accessibility)));
}

FrameActor::FrameActor(DevToolsServer& devtools, String name, WeakPtr<TabActor> tab, WeakPtr<CSSPropertiesActor> css_properties, WeakPtr<ConsoleActor> console, WeakPtr<InspectorActor> inspector, WeakPtr<StyleSheetsActor> style_sheets, WeakPtr<ThreadActor> thread, WeakPtr<AccessibilityActor> accessibility)
    : Actor(devtools, move(name))
    , m_tab(move(tab))
    , m_css_properties(move(css_properties))
    , m_console(move(console))
    , m_inspector(move(inspector))
    , m_style_sheets(move(style_sheets))
    , m_thread(move(thread))
    , m_accessibility(move(accessibility))
{
    if (auto tab = m_tab.strong_ref()) {
        devtools.delegate().listen_for_console_messages(
            tab->description(),
            [weak_self = make_weak_ptr<FrameActor>()](i32 message_index) {
                if (auto self = weak_self.strong_ref())
                    self->console_message_available(message_index);
            },
            [weak_self = make_weak_ptr<FrameActor>()](i32 start_index, Vector<WebView::ConsoleOutput> console_output) {
                if (auto self = weak_self.strong_ref())
                    self->console_messages_received(start_index, move(console_output));
            });

        // FIXME: We should adopt WebContent to inform us when style sheets are available or removed.
        devtools.delegate().retrieve_style_sheets(tab->description(),
            async_handler<FrameActor>({}, [](auto& self, auto style_sheets, auto& response) {
                self.style_sheets_available(response, move(style_sheets));
            }));

        devtools.delegate().listen_for_network_events(
            tab->description(),
            [weak_self = make_weak_ptr<FrameActor>()](DevToolsDelegate::NetworkRequestData data) {
                if (auto self = weak_self.strong_ref())
                    self->on_network_request_started(move(data));
            },
            [weak_self = make_weak_ptr<FrameActor>()](DevToolsDelegate::NetworkResponseData data) {
                if (auto self = weak_self.strong_ref())
                    self->on_network_response_headers_received(move(data));
            },
            [weak_self = make_weak_ptr<FrameActor>()](DevToolsDelegate::NetworkRequestCompleteData data) {
                if (auto self = weak_self.strong_ref())
                    self->on_network_request_finished(move(data));
            });
    }
}

FrameActor::~FrameActor()
{
    if (auto tab = m_tab.strong_ref()) {
        devtools().delegate().stop_listening_for_console_messages(tab->description());
        devtools().delegate().stop_listening_for_network_events(tab->description());
    }
}

void FrameActor::handle_message(Message const& message)
{
    JsonObject response;

    if (message.type == "detach"sv) {
        if (auto tab = m_tab.strong_ref()) {
            devtools().delegate().stop_listening_for_dom_properties(tab->description());
            devtools().delegate().stop_listening_for_dom_mutations(tab->description());
            devtools().delegate().stop_listening_for_console_messages(tab->description());
            devtools().delegate().stop_listening_for_style_sheet_sources(tab->description());
            tab->reset_selected_node();
        }

        send_response(message, move(response));
        return;
    }

    if (message.type == "listFrames"sv) {
        send_response(message, move(response));
        return;
    }

    send_unrecognized_packet_type_error(message);
}

void FrameActor::send_frame_update_message()
{
    JsonArray frames;

    if (auto tab_actor = m_tab.strong_ref()) {
        JsonObject frame;
        frame.set("id"sv, tab_actor->description().id);
        frame.set("title"sv, tab_actor->description().title);
        frame.set("url"sv, tab_actor->description().url);
        frames.must_append(move(frame));
    }

    JsonObject message;
    message.set("type"sv, "frameUpdate"sv);
    message.set("frames"sv, move(frames));
    send_message(move(message));
}

JsonObject FrameActor::serialize_target() const
{
    JsonObject traits;
    traits.set("frames"sv, true);
    traits.set("isBrowsingContext"sv, true);
    traits.set("logInPage"sv, false);
    traits.set("navigation"sv, true);
    traits.set("supportsTopLevelTargetFlag"sv, true);
    traits.set("watchpoints"sv, true);

    JsonObject target;
    target.set("actor"sv, name());
    target.set("targetType"sv, "frame"sv);

    if (auto tab_actor = m_tab.strong_ref()) {
        target.set("title"sv, tab_actor->description().title);
        target.set("url"sv, tab_actor->description().url);
        target.set("browsingContextID"sv, tab_actor->description().id);
        target.set("outerWindowID"sv, tab_actor->description().id);
        target.set("isTopLevelTarget"sv, true);
    }

    target.set("traits"sv, move(traits));

    if (auto accessibility = m_accessibility.strong_ref())
        target.set("accessibilityActor"sv, accessibility->name());
    if (auto console = m_console.strong_ref())
        target.set("consoleActor"sv, console->name());
    if (auto css_properties = m_css_properties.strong_ref())
        target.set("cssPropertiesActor"sv, css_properties->name());
    if (auto inspector = m_inspector.strong_ref())
        target.set("inspectorActor"sv, inspector->name());
    if (auto style_sheets = m_style_sheets.strong_ref())
        target.set("styleSheetsActor"sv, style_sheets->name());
    if (auto thread = m_thread.strong_ref())
        target.set("threadActor"sv, thread->name());

    return target;
}

void FrameActor::style_sheets_available(JsonObject& response, Vector<Web::CSS::StyleSheetIdentifier> style_sheets)
{
    JsonArray sheets;

    String tab_url;
    if (auto tab_actor = m_tab.strong_ref())
        tab_url = tab_actor->description().url;

    auto style_sheets_actor = m_style_sheets.strong_ref();
    if (!style_sheets_actor)
        return;

    for (auto const& [i, style_sheet] : enumerate(style_sheets)) {
        auto resource_id = MUST(String::formatted("{}-stylesheet:{}", style_sheets_actor->name(), i));

        JsonValue href;
        JsonValue source_map_base_url;
        JsonValue title;

        if (style_sheet.url.has_value()) {
            if (style_sheet.type == Web::CSS::StyleSheetIdentifier::Type::UserAgent) {
                // LibWeb sets the URL to a style sheet name for UA style sheets. DevTools would reject these invalid URLs.
                href = MUST(String::formatted("resource://{}", style_sheet.url.value()));
                title = *style_sheet.url;
                source_map_base_url = tab_url;
            } else if (style_sheet.type == Web::CSS::StyleSheetIdentifier::Type::StyleElement) {
                source_map_base_url = *style_sheet.url;
            } else {
                href = *style_sheet.url;
                source_map_base_url = *style_sheet.url;
            }
        } else {
            source_map_base_url = tab_url;
        }

        JsonObject sheet;
        sheet.set("atRules"sv, JsonArray {});
        sheet.set("constructed"sv, false);
        sheet.set("disabled"sv, false);
        sheet.set("fileName"sv, JsonValue {});
        sheet.set("href"sv, move(href));
        sheet.set("isNew"sv, false);
        sheet.set("nodeHref"sv, tab_url);
        sheet.set("resourceId"sv, move(resource_id));
        sheet.set("ruleCount"sv, style_sheet.rule_count);
        sheet.set("sourceMapBaseURL"sv, move(source_map_base_url));
        sheet.set("sourceMapURL"sv, ""sv);
        sheet.set("styleSheetIndex"sv, i);
        sheet.set("system"sv, style_sheet.type == Web::CSS::StyleSheetIdentifier::Type::UserAgent);
        sheet.set("title"sv, move(title));

        sheets.must_append(move(sheet));
    }

    JsonArray stylesheets;
    stylesheets.must_append("stylesheet"sv);
    stylesheets.must_append(move(sheets));

    JsonArray array;
    array.must_append(move(stylesheets));

    response.set("type"sv, "resources-available-array"sv);
    response.set("array"sv, move(array));

    style_sheets_actor->set_style_sheets(move(style_sheets));
}

void FrameActor::console_message_available(i32 message_index)
{
    if (message_index <= m_highest_received_message_index) {
        dbgln("Notified about console message we already have");
        return;
    }
    if (message_index <= m_highest_notified_message_index) {
        dbgln("Notified about console message we're already aware of");
        return;
    }

    m_highest_notified_message_index = message_index;

    if (!m_waiting_for_messages)
        request_console_messages();
}

void FrameActor::console_messages_received(i32 start_index, Vector<WebView::ConsoleOutput> console_output)
{
    auto end_index = start_index + static_cast<i32>(console_output.size()) - 1;
    if (end_index <= m_highest_received_message_index) {
        dbgln("Received old console messages");
        return;
    }

    JsonArray console_messages;
    JsonArray error_messages;

    for (auto& output : console_output) {
        JsonObject message;

        output.output.visit(
            [&](WebView::ConsoleLog& log) {
                switch (log.level) {
                case JS::Console::LogLevel::Debug:
                    message.set("level"sv, "debug"sv);
                    break;
                case JS::Console::LogLevel::Error:
                    message.set("level"sv, "error"sv);
                    break;
                case JS::Console::LogLevel::Info:
                    message.set("level"sv, "info"sv);
                    break;
                case JS::Console::LogLevel::Log:
                    message.set("level"sv, "log"sv);
                    break;
                case JS::Console::LogLevel::Warn:
                    message.set("level"sv, "warn"sv);
                    break;
                default:
                    // FIXME: Implement remaining console levels.
                    return;
                }

                message.set("filename"sv, "<eval>"sv);
                message.set("lineNumber"sv, 1);
                message.set("columnNumber"sv, 1);
                message.set("timeStamp"sv, output.timestamp.milliseconds_since_epoch());
                message.set("arguments"sv, JsonArray { move(log.arguments) });

                console_messages.must_append(move(message));
            },
            [&](WebView::ConsoleError const& error) {
                StringBuilder stack;

                for (auto const& frame : error.trace) {
                    if (frame.function.has_value())
                        stack.append(*frame.function);
                    stack.append('@');
                    stack.append(frame.file.map([](auto const& file) -> StringView { return file; }).value_or("unknown"sv));
                    stack.appendff(":{}:{}\n", frame.line.value_or(0), frame.column.value_or(0));
                }

                JsonObject preview;
                preview.set("kind"sv, "Error"sv);
                preview.set("message"sv, error.message);
                preview.set("name"sv, error.name);
                if (!stack.is_empty())
                    preview.set("stack"sv, MUST(stack.to_string()));

                JsonObject exception;
                exception.set("class"sv, error.name);
                exception.set("isError"sv, true);
                exception.set("preview"sv, move(preview));

                JsonObject page_error;
                page_error.set("error"sv, true);
                page_error.set("exception"sv, move(exception));
                page_error.set("hasException"sv, !error.trace.is_empty());
                page_error.set("isPromiseRejection"sv, error.inside_promise);
                page_error.set("timeStamp"sv, output.timestamp.milliseconds_since_epoch());

                message.set("pageError"sv, move(page_error));
                error_messages.must_append(move(message));
            });
    }

    JsonArray array;

    if (!console_messages.is_empty()) {
        JsonArray console_message;
        console_message.must_append("console-message"sv);
        console_message.must_append(move(console_messages));

        array.must_append(move(console_message));
    }
    if (!error_messages.is_empty()) {
        JsonArray error_message;
        error_message.must_append("error-message"sv);
        error_message.must_append(move(error_messages));

        array.must_append(move(error_message));
    }

    JsonObject message;
    message.set("type"sv, "resources-available-array"sv);
    message.set("array"sv, move(array));
    send_message(move(message));

    m_highest_received_message_index = end_index;
    m_waiting_for_messages = false;

    if (m_highest_received_message_index < m_highest_notified_message_index)
        request_console_messages();
}

void FrameActor::request_console_messages()
{
    VERIFY(!m_waiting_for_messages);

    if (auto tab = m_tab.strong_ref()) {
        devtools().delegate().request_console_messages(m_tab->description(), m_highest_received_message_index + 1);
        m_waiting_for_messages = true;
    }
}

void FrameActor::on_network_request_started(DevToolsDelegate::NetworkRequestData data)
{
    auto& actor = devtools().register_actor<NetworkEventActor>(data.request_id);
    actor.set_request_info(move(data.url), move(data.method), data.start_time, move(data.request_headers));
    m_network_events.set(data.request_id, actor);

    JsonArray events;
    events.must_append(actor.serialize_initial_event());

    JsonArray network_event;
    network_event.must_append("network-event"sv);
    network_event.must_append(move(events));

    JsonArray array;
    array.must_append(move(network_event));

    JsonObject message;
    message.set("type"sv, "resources-available-array"sv);
    message.set("array"sv, move(array));
    send_message(move(message));
}

void FrameActor::on_network_response_headers_received(DevToolsDelegate::NetworkResponseData data)
{
    auto it = m_network_events.find(data.request_id);
    if (it == m_network_events.end())
        return;

    auto& actor = *it->value;
    actor.set_response_start(data.status_code, data.reason_phrase);

    // Extract Content-Type before moving headers
    String mime_type;
    i64 headers_size = 0;
    for (auto const& header : data.response_headers) {
        headers_size += static_cast<i64>(header.name.bytes().size() + header.value.bytes().size() + 4);
        if (header.name.equals_ignoring_ascii_case("content-type"sv))
            mime_type = MUST(String::from_byte_string(header.value));
    }

    actor.set_response_headers(move(data.response_headers));

    // Build resource updates object
    JsonObject resource_updates;
    resource_updates.set("status"sv, String::number(data.status_code));
    resource_updates.set("statusText"sv, data.reason_phrase.value_or(String {}));
    resource_updates.set("headersSize"sv, headers_size);
    resource_updates.set("mimeType"sv, mime_type);
    // FIXME: Get actual HTTP version from response
    resource_updates.set("httpVersion"sv, "HTTP/1.1"sv);
    // FIXME: Get actual remote address and port from connection
    resource_updates.set("remoteAddress"sv, String {});
    resource_updates.set("remotePort"sv, 0);
    // FIXME: Calculate actual waiting time (time between request sent and first byte received)
    resource_updates.set("waitingTime"sv, 0);
    // Mark headers as available
    resource_updates.set("responseHeadersAvailable"sv, true);

    JsonObject update_entry;
    update_entry.set("resourceId"sv, static_cast<i64>(data.request_id));
    update_entry.set("resourceType"sv, "network-event"sv);
    update_entry.set("resourceUpdates"sv, move(resource_updates));
    update_entry.set("browsingContextID"sv, 1);
    update_entry.set("innerWindowId"sv, 1);

    JsonArray updates;
    updates.must_append(move(update_entry));

    JsonArray network_event_updates;
    network_event_updates.must_append("network-event"sv);
    network_event_updates.must_append(move(updates));

    JsonArray array;
    array.must_append(move(network_event_updates));

    JsonObject message;
    message.set("type"sv, "resources-updated-array"sv);
    message.set("array"sv, move(array));
    send_message(move(message));
}

void FrameActor::on_network_request_finished(DevToolsDelegate::NetworkRequestCompleteData data)
{
    auto it = m_network_events.find(data.request_id);
    if (it == m_network_events.end())
        return;

    auto& actor = *it->value;
    actor.set_request_complete(data.body_size, data.timing_info, data.network_error);

    // Calculate total time in milliseconds
    auto total_time = (data.timing_info.response_end_microseconds - data.timing_info.request_start_microseconds) / 1000;

    // Build resource updates object with content and timing info
    JsonObject resource_updates;
    resource_updates.set("contentSize"sv, static_cast<i64>(data.body_size));
    resource_updates.set("transferredSize"sv, static_cast<i64>(data.body_size));
    resource_updates.set("totalTime"sv, total_time);
    // Mark as complete
    resource_updates.set("responseContentAvailable"sv, true);
    resource_updates.set("eventTimingsAvailable"sv, true);

    JsonObject update_entry;
    update_entry.set("resourceId"sv, static_cast<i64>(data.request_id));
    update_entry.set("resourceType"sv, "network-event"sv);
    update_entry.set("resourceUpdates"sv, move(resource_updates));
    update_entry.set("browsingContextID"sv, 1);
    update_entry.set("innerWindowId"sv, 1);

    JsonArray updates;
    updates.must_append(move(update_entry));

    JsonArray network_event_updates;
    network_event_updates.must_append("network-event"sv);
    network_event_updates.must_append(move(updates));

    JsonArray array;
    array.must_append(move(network_event_updates));

    JsonObject message;
    message.set("type"sv, "resources-updated-array"sv);
    message.set("array"sv, move(array));
    send_message(move(message));
}

}
