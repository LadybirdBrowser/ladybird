/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Enumerate.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <LibDevTools/Actors/CSSPropertiesActor.h>
#include <LibDevTools/Actors/ConsoleActor.h>
#include <LibDevTools/Actors/FrameActor.h>
#include <LibDevTools/Actors/InspectorActor.h>
#include <LibDevTools/Actors/StyleSheetsActor.h>
#include <LibDevTools/Actors/TabActor.h>
#include <LibDevTools/Actors/ThreadActor.h>
#include <LibDevTools/DevToolsDelegate.h>
#include <LibDevTools/DevToolsServer.h>
#include <LibWebView/ConsoleOutput.h>

namespace DevTools {

NonnullRefPtr<FrameActor> FrameActor::create(DevToolsServer& devtools, String name, WeakPtr<TabActor> tab, WeakPtr<CSSPropertiesActor> css_properties, WeakPtr<ConsoleActor> console, WeakPtr<InspectorActor> inspector, WeakPtr<StyleSheetsActor> style_sheets, WeakPtr<ThreadActor> thread)
{
    return adopt_ref(*new FrameActor(devtools, move(name), move(tab), move(css_properties), move(console), move(inspector), move(style_sheets), move(thread)));
}

FrameActor::FrameActor(DevToolsServer& devtools, String name, WeakPtr<TabActor> tab, WeakPtr<CSSPropertiesActor> css_properties, WeakPtr<ConsoleActor> console, WeakPtr<InspectorActor> inspector, WeakPtr<StyleSheetsActor> style_sheets, WeakPtr<ThreadActor> thread)
    : Actor(devtools, move(name))
    , m_tab(move(tab))
    , m_css_properties(move(css_properties))
    , m_console(move(console))
    , m_inspector(move(inspector))
    , m_style_sheets(move(style_sheets))
    , m_thread(move(thread))
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
    }
}

FrameActor::~FrameActor()
{
    if (auto tab = m_tab.strong_ref())
        devtools().delegate().stop_listening_for_console_messages(tab->description());
}

void FrameActor::handle_message(Message const& message)
{
    JsonObject response;

    if (message.type == "detach"_sv) {
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

    if (message.type == "listFrames"_sv) {
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
        frame.set("id"_sv, tab_actor->description().id);
        frame.set("title"_sv, tab_actor->description().title);
        frame.set("url"_sv, tab_actor->description().url);
        frames.must_append(move(frame));
    }

    JsonObject message;
    message.set("type"_sv, "frameUpdate"_sv);
    message.set("frames"_sv, move(frames));
    send_message(move(message));
}

JsonObject FrameActor::serialize_target() const
{
    JsonObject traits;
    traits.set("frames"_sv, true);
    traits.set("isBrowsingContext"_sv, true);
    traits.set("logInPage"_sv, false);
    traits.set("navigation"_sv, true);
    traits.set("supportsTopLevelTargetFlag"_sv, true);
    traits.set("watchpoints"_sv, true);

    JsonObject target;
    target.set("actor"_sv, name());
    target.set("targetType"_sv, "frame"_sv);

    if (auto tab_actor = m_tab.strong_ref()) {
        target.set("title"_sv, tab_actor->description().title);
        target.set("url"_sv, tab_actor->description().url);
        target.set("browsingContextID"_sv, tab_actor->description().id);
        target.set("outerWindowID"_sv, tab_actor->description().id);
        target.set("isTopLevelTarget"_sv, true);
    }

    target.set("traits"_sv, move(traits));

    if (auto css_properties = m_css_properties.strong_ref())
        target.set("cssPropertiesActor"_sv, css_properties->name());
    if (auto console = m_console.strong_ref())
        target.set("consoleActor"_sv, console->name());
    if (auto inspector = m_inspector.strong_ref())
        target.set("inspectorActor"_sv, inspector->name());
    if (auto style_sheets = m_style_sheets.strong_ref())
        target.set("styleSheetsActor"_sv, style_sheets->name());
    if (auto thread = m_thread.strong_ref())
        target.set("threadActor"_sv, thread->name());

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
            // LibWeb sets the URL to a style sheet name for UA style sheets. DevTools would reject these invalid URLs.
            if (style_sheet.type == Web::CSS::StyleSheetIdentifier::Type::UserAgent) {
                title = *style_sheet.url;
                source_map_base_url = tab_url;
            } else {
                href = *style_sheet.url;
                source_map_base_url = *style_sheet.url;
            }
        } else {
            source_map_base_url = tab_url;
        }

        JsonObject sheet;
        sheet.set("atRules"_sv, JsonArray {});
        sheet.set("constructed"_sv, false);
        sheet.set("disabled"_sv, false);
        sheet.set("fileName"_sv, JsonValue {});
        sheet.set("href"_sv, move(href));
        sheet.set("isNew"_sv, false);
        sheet.set("nodeHref"_sv, tab_url);
        sheet.set("resourceId"_sv, move(resource_id));
        sheet.set("ruleCount"_sv, style_sheet.rule_count);
        sheet.set("sourceMapBaseURL"_sv, move(source_map_base_url));
        sheet.set("sourceMapURL"_sv, ""_sv);
        sheet.set("styleSheetIndex"_sv, i);
        sheet.set("system"_sv, false);
        sheet.set("title"_sv, move(title));

        sheets.must_append(move(sheet));
    }

    JsonArray stylesheets;
    stylesheets.must_append("stylesheet"_sv);
    stylesheets.must_append(move(sheets));

    JsonArray array;
    array.must_append(move(stylesheets));

    response.set("type"_sv, "resources-available-array"_sv);
    response.set("array"_sv, move(array));

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
                    message.set("level"_sv, "debug"_sv);
                    break;
                case JS::Console::LogLevel::Error:
                    message.set("level"_sv, "error"_sv);
                    break;
                case JS::Console::LogLevel::Info:
                    message.set("level"_sv, "info"_sv);
                    break;
                case JS::Console::LogLevel::Log:
                    message.set("level"_sv, "log"_sv);
                    break;
                case JS::Console::LogLevel::Warn:
                    message.set("level"_sv, "warn"_sv);
                    break;
                default:
                    // FIXME: Implement remaining console levels.
                    return;
                }

                message.set("filename"_sv, "<eval>"_sv);
                message.set("lineNumber"_sv, 1);
                message.set("columnNumber"_sv, 1);
                message.set("timeStamp"_sv, output.timestamp.milliseconds_since_epoch());
                message.set("arguments"_sv, JsonArray { move(log.arguments) });

                console_messages.must_append(move(message));
            },
            [&](WebView::ConsoleError const& error) {
                StringBuilder stack;

                for (auto const& frame : error.trace) {
                    if (frame.function.has_value())
                        stack.append(*frame.function);
                    stack.append('@');
                    stack.append(frame.file.map([](auto const& file) -> StringView { return file; }).value_or("unknown"_sv));
                    stack.appendff(":{}:{}\n", frame.line.value_or(0), frame.column.value_or(0));
                }

                JsonObject preview;
                preview.set("kind"_sv, "Error"_sv);
                preview.set("message"_sv, error.message);
                preview.set("name"_sv, error.name);
                if (!stack.is_empty())
                    preview.set("stack"_sv, MUST(stack.to_string()));

                JsonObject exception;
                exception.set("class"_sv, error.name);
                exception.set("isError"_sv, true);
                exception.set("preview"_sv, move(preview));

                JsonObject page_error;
                page_error.set("error"_sv, true);
                page_error.set("exception"_sv, move(exception));
                page_error.set("hasException"_sv, !error.trace.is_empty());
                page_error.set("isPromiseRejection"_sv, error.inside_promise);
                page_error.set("timeStamp"_sv, output.timestamp.milliseconds_since_epoch());

                message.set("pageError"_sv, move(page_error));
                error_messages.must_append(move(message));
            });
    }

    JsonArray array;

    if (!console_messages.is_empty()) {
        JsonArray console_message;
        console_message.must_append("console-message"_sv);
        console_message.must_append(move(console_messages));

        array.must_append(move(console_message));
    }
    if (!error_messages.is_empty()) {
        JsonArray error_message;
        error_message.must_append("error-message"_sv);
        error_message.must_append(move(error_messages));

        array.must_append(move(error_message));
    }

    JsonObject message;
    message.set("type"_sv, "resources-available-array"_sv);
    message.set("array"_sv, move(array));
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

}
