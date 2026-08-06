/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibDevTools/Actors/DebuggerFrameActor.h>

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
        // FIXME: Return the frame's lexical environment once paused scopes are exposed by LibJS.
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

    JsonObject this_value;
    this_value.set("type"sv, "undefined"sv);

    JsonObject frame;
    frame.set("actor"sv, name());
    frame.set("type"sv, "call"sv);
    frame.set("state"sv, "on-stack"sv);
    frame.set("displayName"sv, m_frame.display_name.to_utf8());
    frame.set("arguments"sv, JsonArray {});
    frame.set("this"sv, move(this_value));
    frame.set("where"sv, move(location));
    frame.set("oldest"sv, m_oldest);
    return frame;
}

}
