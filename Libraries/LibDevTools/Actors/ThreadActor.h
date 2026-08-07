/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/NonnullRefPtr.h>
#include <LibDevTools/Actor.h>
#include <LibDevTools/DevToolsDelegate.h>
#include <LibDevTools/Forward.h>

namespace DevTools {

class DEVTOOLS_API ThreadActor final : public Actor {
public:
    enum class ObjectPropertiesRequest {
        Iterator,
        Property,
        Prototype,
        PrototypeAndProperties,
    };

    static constexpr auto base_name = "thread"sv;

    static NonnullRefPtr<ThreadActor> create(DevToolsServer&, String name, WeakPtr<TabActor>, WeakPtr<WatcherActor>);
    virtual ~ThreadActor() override;

    JsonObject serialize_source(Web::HTML::ScriptRegistry::Description const&);
    JsonArray serialize_sources(Vector<Web::HTML::ScriptRegistry::Description> const&);
    JsonValue serialize_debugger_value(WebView::DebuggerValue const&);
    Optional<u64> frame_id_for_actor(StringView actor) const;
    void get_frame_environment(DebuggerFrameActor&, Actor::Message const&, u64 frame_id);
    void get_object_properties(ObjectActor&, Actor::Message const&, ObjectPropertiesRequest, Optional<String> property_name = {});
    void get_object_symbols(ObjectActor&, Actor::Message const&);
    void did_pause(WebView::DebuggerPause);
    void did_resume();
    void release_pause_actor(Actor&);
    void resume(WebView::DebuggerResumeMode);
    bool is_paused_in_source(Web::HTML::ScriptRegistry::Identifier source_id) const { return m_paused_source_id == source_id; }

private:
    ThreadActor(DevToolsServer&, String name, WeakPtr<TabActor>, WeakPtr<WatcherActor>);

    virtual void handle_message(Message const&) override;

    void prune_source_actors(Vector<Web::HTML::ScriptRegistry::Description> const&);
    SourceActor& source_actor_for(Web::HTML::ScriptRegistry::Description const&);
    ObjectActor& object_actor_for(WebView::DebuggerValue const&);
    JsonObject serialize_environment_chain(Vector<WebView::DebuggerEnvironment>);
    JsonObject serialize_property_descriptor(WebView::DebuggerProperty const&);
    void clear_pause_actors();
    void attach();
    void send_wrong_state_error(Message const&, StringView message);

    WeakPtr<TabActor> m_tab;
    WeakPtr<WatcherActor> m_watcher;
    HashMap<Web::HTML::ScriptRegistry::Identifier, WeakPtr<SourceActor>> m_source_actors;
    HashMap<u64, WeakPtr<ObjectActor>> m_object_actors;
    Vector<WeakPtr<DebuggerFrameActor>> m_frame_actors;
    Vector<WeakPtr<Actor>> m_pause_scoped_actors;
    Optional<Web::HTML::ScriptRegistry::Identifier> m_paused_source_id;
    bool m_is_paused { false };
    bool m_pause_requested_on_next { false };
};

}
