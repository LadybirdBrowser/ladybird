/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <LibDevTools/Actors/CSSPropertiesActor.h>
#include <LibDevTools/Actors/FrameActor.h>
#include <LibDevTools/Actors/InspectorActor.h>
#include <LibDevTools/Actors/TabActor.h>
#include <LibDevTools/Actors/ThreadActor.h>

namespace DevTools {

NonnullRefPtr<FrameActor> FrameActor::create(DevToolsServer& devtools, ByteString name, WeakPtr<TabActor> tab, WeakPtr<CSSPropertiesActor> css_properties, WeakPtr<InspectorActor> inspector, WeakPtr<ThreadActor> thread)
{
    return adopt_ref(*new FrameActor(devtools, move(name), move(tab), move(css_properties), move(inspector), move(thread)));
}

FrameActor::FrameActor(DevToolsServer& devtools, ByteString name, WeakPtr<TabActor> tab, WeakPtr<CSSPropertiesActor> css_properties, WeakPtr<InspectorActor> inspector, WeakPtr<ThreadActor> thread)
    : Actor(devtools, move(name))
    , m_tab(move(tab))
    , m_css_properties(move(css_properties))
    , m_inspector(move(inspector))
    , m_thread(move(thread))
{
}

FrameActor::~FrameActor() = default;

void FrameActor::handle_message(StringView type, JsonObject const&)
{
    JsonObject response;
    response.set("from"sv, name());

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
    if (auto inspector = m_inspector.strong_ref())
        target.set("inspectorActor"sv, inspector->name());
    if (auto thread = m_thread.strong_ref())
        target.set("threadActor"sv, thread->name());

    return target;
}

}
