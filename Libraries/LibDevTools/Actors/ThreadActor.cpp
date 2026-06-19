/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <LibDevTools/Actors/SourceActor.h>
#include <LibDevTools/Actors/TabActor.h>
#include <LibDevTools/Actors/ThreadActor.h>
#include <LibDevTools/DevToolsServer.h>

namespace DevTools {

NonnullRefPtr<ThreadActor> ThreadActor::create(DevToolsServer& devtools, String name, WeakPtr<TabActor> tab)
{
    return adopt_ref(*new ThreadActor(devtools, move(name), move(tab)));
}

ThreadActor::ThreadActor(DevToolsServer& devtools, String name, WeakPtr<TabActor> tab)
    : Actor(devtools, move(name))
    , m_tab(move(tab))
{
}

ThreadActor::~ThreadActor()
{
    for (auto const& actor : m_source_actors) {
        if (auto source_actor = actor.value.strong_ref())
            devtools().unregister_actor(source_actor->name());
    }
}

void ThreadActor::handle_message(Message const& message)
{
    if (message.type == "attach"sv || message.type == "reconfigure"sv || message.type == "skipBreakpoints"sv || message.type == "resume"sv) {
        JsonObject response;
        send_response(message, move(response));
        return;
    }

    if (message.type == "getAvailableEventBreakpoints"sv) {
        JsonObject response;
        JsonArray breakpoints;
        response.set("value"sv, move(breakpoints));
        send_response(message, move(response));
        return;
    }

    if (message.type == "sources"sv) {
        auto tab = m_tab.strong_ref();
        if (!tab) {
            JsonObject response;
            JsonArray sources;
            response.set("sources"sv, move(sources));
            send_response(message, move(response));
            return;
        }

        devtools().delegate().retrieve_sources(tab->description(),
            async_handler<ThreadActor>(message, [](auto& self, auto sources, auto& response) {
                response.set("sources"sv, self.serialize_sources(sources));
            }));
        return;
    }

    send_unrecognized_packet_type_error(message);
}

JsonObject ThreadActor::serialize_source(Web::HTML::ScriptRegistry::Description const& source)
{
    return source_actor_for(source).serialize_source();
}

JsonArray ThreadActor::serialize_sources(Vector<Web::HTML::ScriptRegistry::Description> const& sources)
{
    prune_source_actors(sources);

    JsonArray serialized_sources;
    for (auto const& source : sources)
        serialized_sources.must_append(serialize_source(source));
    return serialized_sources;
}

void ThreadActor::prune_source_actors(Vector<Web::HTML::ScriptRegistry::Description> const& sources)
{
    HashTable<Web::HTML::ScriptRegistry::Identifier> current_sources;
    for (auto const& source : sources)
        current_sources.set(source.id);

    Vector<Web::HTML::ScriptRegistry::Identifier> stale_sources;
    for (auto const& actor : m_source_actors) {
        if (!current_sources.contains(actor.key))
            stale_sources.append(actor.key);
    }

    for (auto const& source_id : stale_sources) {
        auto actor = m_source_actors.take(source_id);
        if (actor.has_value()) {
            if (auto source_actor = actor->strong_ref())
                devtools().unregister_actor(source_actor->name());
        }
    }
}

SourceActor& ThreadActor::source_actor_for(Web::HTML::ScriptRegistry::Description const& source)
{
    if (auto actor = m_source_actors.find(source.id); actor != m_source_actors.end()) {
        if (auto source_actor = actor->value.strong_ref())
            return *source_actor;
    }

    auto& actor = devtools().register_actor<SourceActor>(m_tab, source);
    m_source_actors.set(source.id, actor);
    return actor;
}

}
