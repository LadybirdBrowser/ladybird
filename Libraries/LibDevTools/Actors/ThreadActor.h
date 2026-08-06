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
    static constexpr auto base_name = "thread"sv;

    static NonnullRefPtr<ThreadActor> create(DevToolsServer&, String name, WeakPtr<TabActor>, WeakPtr<WatcherActor>);
    virtual ~ThreadActor() override;

    JsonObject serialize_source(Web::HTML::ScriptRegistry::Description const&);
    JsonArray serialize_sources(Vector<Web::HTML::ScriptRegistry::Description> const&);
    void did_pause(WebView::DebuggerPause);

private:
    ThreadActor(DevToolsServer&, String name, WeakPtr<TabActor>, WeakPtr<WatcherActor>);

    virtual void handle_message(Message const&) override;

    void prune_source_actors(Vector<Web::HTML::ScriptRegistry::Description> const&);
    SourceActor& source_actor_for(Web::HTML::ScriptRegistry::Description const&);
    void clear_pause_actors();
    void attach();
    void did_resume();
    void resume();
    void send_wrong_state_error(Message const&, StringView message);

    WeakPtr<TabActor> m_tab;
    WeakPtr<WatcherActor> m_watcher;
    HashMap<Web::HTML::ScriptRegistry::Identifier, WeakPtr<SourceActor>> m_source_actors;
    Vector<WeakPtr<DebuggerFrameActor>> m_pause_actors;
    bool m_is_paused { false };
    bool m_pause_requested_on_next { false };
};

}
