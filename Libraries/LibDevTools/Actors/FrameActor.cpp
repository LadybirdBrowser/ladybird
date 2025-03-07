/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <LibDevTools/Actors/CSSPropertiesActor.h>
#include <LibDevTools/Actors/ConsoleActor.h>
#include <LibDevTools/Actors/FrameActor.h>
#include <LibDevTools/Actors/InspectorActor.h>
#include <LibDevTools/Actors/TabActor.h>
#include <LibDevTools/Actors/ThreadActor.h>
#include <LibDevTools/DevToolsDelegate.h>
#include <LibDevTools/DevToolsServer.h>
#include <LibWebView/ConsoleOutput.h>

namespace DevTools {

NonnullRefPtr<FrameActor> FrameActor::create(DevToolsServer& devtools, String name, WeakPtr<TabActor> tab, WeakPtr<CSSPropertiesActor> css_properties, WeakPtr<ConsoleActor> console, WeakPtr<InspectorActor> inspector, WeakPtr<ThreadActor> thread)
{
    return adopt_ref(*new FrameActor(devtools, move(name), move(tab), move(css_properties), move(console), move(inspector), move(thread)));
}

FrameActor::FrameActor(DevToolsServer& devtools, String name, WeakPtr<TabActor> tab, WeakPtr<CSSPropertiesActor> css_properties, WeakPtr<ConsoleActor> console, WeakPtr<InspectorActor> inspector, WeakPtr<ThreadActor> thread)
    : Actor(devtools, move(name))
    , m_tab(move(tab))
    , m_css_properties(move(css_properties))
    , m_console(move(console))
    , m_inspector(move(inspector))
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
    }
}

FrameActor::~FrameActor()
{
    if (auto tab = m_tab.strong_ref())
        devtools().delegate().stop_listening_for_console_messages(tab->description());
}

void FrameActor::handle_message(StringView type, JsonObject const&)
{
    JsonObject response;
    response.set("from"sv, name());

    if (type == "detach"sv) {
        if (auto tab = m_tab.strong_ref()) {
            devtools().delegate().stop_listening_for_dom_mutations(tab->description());
            devtools().delegate().stop_listening_for_console_messages(tab->description());
            tab->reset_selected_node();
        }

        send_message(move(response));
        return;
    }

    if (type == "listFrames"sv) {
        send_message(move(response));
        return;
    }

    send_unrecognized_packet_type_error(type);
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
    message.set("from"sv, name());
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

    if (auto tab_actor = m_tab.strong_ref()) {
        target.set("title"sv, tab_actor->description().title);
        target.set("url"sv, tab_actor->description().url);
        target.set("browsingContextID"sv, tab_actor->description().id);
        target.set("outerWindowID"sv, tab_actor->description().id);
        target.set("isTopLevelTarget"sv, true);
    }

    target.set("traits"sv, move(traits));

    if (auto css_properties = m_css_properties.strong_ref())
        target.set("cssPropertiesActor"sv, css_properties->name());
    if (auto console = m_console.strong_ref())
        target.set("consoleActor"sv, console->name());
    if (auto inspector = m_inspector.strong_ref())
        target.set("inspectorActor"sv, inspector->name());
    if (auto thread = m_thread.strong_ref())
        target.set("threadActor"sv, thread->name());

    return target;
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

    JsonArray messages;
    messages.ensure_capacity(console_output.size());

    for (auto& output : console_output) {
        JsonObject message;

        switch (output.level) {
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
            continue;
        }

        message.set("filename"sv, "<eval>"sv);
        message.set("line_number"sv, 1);
        message.set("column_number"sv, 1);
        message.set("time_stamp"sv, output.timestamp.milliseconds_since_epoch());
        message.set("arguments"sv, JsonArray { move(output.arguments) });

        messages.must_append(move(message));
    }

    JsonArray console_message;
    console_message.must_append("console-message"sv);
    console_message.must_append(move(messages));

    JsonArray array;
    array.must_append(move(console_message));

    JsonObject message;
    message.set("from"sv, name());
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

}
