/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibDevTools/Actors/DebuggerFrameActor.h>
#include <LibDevTools/Actors/ThreadActor.h>

namespace DevTools {

NonnullRefPtr<DebuggerFrameActor> DebuggerFrameActor::create(DevToolsServer& devtools, String name, WeakPtr<ThreadActor> thread, WebView::DebuggerFrame frame, String source_actor, bool oldest)
{
    return adopt_ref(*new DebuggerFrameActor(devtools, move(name), move(thread), move(frame), move(source_actor), oldest));
}

DebuggerFrameActor::DebuggerFrameActor(DevToolsServer& devtools, String name, WeakPtr<ThreadActor> thread, WebView::DebuggerFrame frame, String source_actor, bool oldest)
    : Actor(devtools, move(name))
    , m_thread(move(thread))
    , m_frame(move(frame))
    , m_source_actor(move(source_actor))
    , m_oldest(oldest)
{
}

DebuggerFrameActor::~DebuggerFrameActor() = default;

void DebuggerFrameActor::handle_message(Message const& message)
{
    if (message.type == "getEnvironment"sv) {
        if (auto thread = m_thread.strong_ref())
            thread->get_frame_environment(*this, message, m_frame.id);
        else
            send_response(message, {});
        return;
    }

    send_unrecognized_packet_type_error(message);
}

JsonObject DebuggerFrameActor::serialize_frame() const
{
    JsonObject location;
    location.set("actor"sv, m_source_actor);
    location.set("line"sv, m_frame.location.line);
    location.set("column"sv, m_frame.location.column);

    JsonObject frame;
    frame.set("actor"sv, name());
    frame.set("type"sv, "call"sv);
    frame.set("state"sv, "on-stack"sv);
    frame.set("displayName"sv, m_frame.display_name.to_utf8());
    JsonArray arguments;
    if (auto thread = m_thread.strong_ref()) {
        for (auto const& argument : m_frame.arguments)
            arguments.must_append(thread->serialize_debugger_value(argument));
        frame.set("this"sv, thread->serialize_debugger_value(m_frame.this_value));
    } else {
        JsonObject undefined;
        undefined.set("type"sv, "undefined"sv);
        frame.set("this"sv, move(undefined));
    }
    frame.set("arguments"sv, move(arguments));
    frame.set("where"sv, move(location));
    frame.set("oldest"sv, m_oldest);
    return frame;
}

}
