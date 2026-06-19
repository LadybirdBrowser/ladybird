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

    static NonnullRefPtr<ThreadActor> create(DevToolsServer&, String name, WeakPtr<TabActor>);
    virtual ~ThreadActor() override;

    JsonObject serialize_source(Web::HTML::ScriptRegistry::Description const&);
    JsonArray serialize_sources(Vector<Web::HTML::ScriptRegistry::Description> const&);

private:
    ThreadActor(DevToolsServer&, String name, WeakPtr<TabActor>);

    virtual void handle_message(Message const&) override;

    void prune_source_actors(Vector<Web::HTML::ScriptRegistry::Description> const&);
    SourceActor& source_actor_for(Web::HTML::ScriptRegistry::Description const&);

    WeakPtr<TabActor> m_tab;
    HashMap<Web::HTML::ScriptRegistry::Identifier, WeakPtr<SourceActor>> m_source_actors;
};

}
