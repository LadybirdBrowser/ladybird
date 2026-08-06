/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <LibDevTools/Actors/DebuggerFrameActor.h>
#include <LibDevTools/Actors/SourceActor.h>
#include <LibDevTools/Actors/TabActor.h>
#include <LibDevTools/Actors/ThreadActor.h>
#include <LibDevTools/Actors/WatcherActor.h>
#include <LibDevTools/DevToolsServer.h>

namespace DevTools {

NonnullRefPtr<ThreadActor> ThreadActor::create(DevToolsServer& devtools, String name, WeakPtr<TabActor> tab, WeakPtr<WatcherActor> watcher)
{
    return adopt_ref(*new ThreadActor(devtools, move(name), move(tab), move(watcher)));
}

ThreadActor::ThreadActor(DevToolsServer& devtools, String name, WeakPtr<TabActor> tab, WeakPtr<WatcherActor> watcher)
    : Actor(devtools, move(name))
    , m_tab(move(tab))
    , m_watcher(move(watcher))
{
}

ThreadActor::~ThreadActor()
{
    clear_pause_actors();
    for (auto const& actor : m_source_actors) {
        if (auto source_actor = actor.value.strong_ref())
            devtools().unregister_actor(source_actor->name());
    }
}

void ThreadActor::handle_message(Message const& message)
{
    if (message.type == "attach"sv) {
        attach();
        JsonObject response;
        send_response(message, move(response));
        return;
    }

    if (message.type == "resume"sv) {
        resume();
        JsonObject response;
        send_response(message, move(response));
        return;
    }

    if (message.type == "interrupt"sv) {
        auto when = get_required_parameter<String>(message, "when"sv);
        if (!when.has_value())
            return;
        if (*when != "onNext"sv) {
            JsonObject response;
            response.set("error"sv, "unsupported"sv);
            response.set("message"sv, "Only interrupting on the next bytecode execution is supported"sv);
            send_response(message, move(response));
            return;
        }
        if (m_is_paused) {
            send_wrong_state_error(message, "Cannot interrupt while the thread is paused"sv);
            return;
        }

        auto tab = m_tab.strong_ref();
        if (!tab) {
            send_wrong_state_error(message, "Cannot interrupt a detached thread"sv);
            return;
        }

        m_pause_requested_on_next = true;
        devtools().delegate().interrupt_debugger(tab->description());
        send_response(message, {});
        return;
    }

    if (message.type == "reconfigure"sv || message.type == "skipBreakpoints"sv) {
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

void ThreadActor::attach()
{
    if (auto watcher = m_watcher.strong_ref())
        watcher->start_watching_thread_state_resources();
}

void ThreadActor::resume()
{
    if (!m_is_paused)
        return;

    if (auto tab = m_tab.strong_ref())
        devtools().delegate().resume_debugger(tab->description());

    did_resume();
}

void ThreadActor::did_resume()
{
    if (!m_is_paused)
        return;

    m_is_paused = false;
    clear_pause_actors();
    if (auto watcher = m_watcher.strong_ref()) {
        JsonObject resource;
        resource.set("state"sv, "resumed"sv);
        watcher->send_thread_state_available_message(move(resource));
    }
}

void ThreadActor::did_pause(WebView::DebuggerPause pause)
{
    auto pause_was_requested_on_next = m_pause_requested_on_next;
    m_pause_requested_on_next = false;
    clear_pause_actors();
    if (pause.frames.is_empty()) {
        if (auto tab = m_tab.strong_ref())
            devtools().delegate().resume_debugger(tab->description());
        did_resume();
        return;
    }

    auto frame = pause.frames.take_first();
    auto& source_actor = source_actor_for(frame.location.source);
    auto& frame_actor = devtools().register_actor<DebuggerFrameActor>(make_weak_ptr<ThreadActor>(), move(frame), source_actor.name());
    add_child_actor(frame_actor);
    m_pause_actors.append(frame_actor);

    auto reason = [&] {
        switch (pause.reason) {
        case WebView::DebuggerPauseReason::Breakpoint:
            return "breakpoint"sv;
        case WebView::DebuggerPauseReason::DebuggerStatement:
            return "debuggerStatement"sv;
        case WebView::DebuggerPauseReason::Entry:
            return "interrupted"sv;
        }
        VERIFY_NOT_REACHED();
    }();

    JsonObject why;
    why.set("type"sv, reason);
    if (pause.reason == WebView::DebuggerPauseReason::Entry && pause_was_requested_on_next)
        why.set("onNext"sv, true);

    JsonObject resource;
    resource.set("state"sv, "paused"sv);
    resource.set("why"sv, move(why));
    resource.set("frame"sv, frame_actor.serialize_frame());

    m_is_paused = true;
    if (auto watcher = m_watcher.strong_ref())
        watcher->send_thread_state_available_message(move(resource));
}

void ThreadActor::clear_pause_actors()
{
    for (auto const& weak_actor : m_pause_actors) {
        if (auto actor = weak_actor.strong_ref()) {
            unregister_child_actor(*actor);
        }
    }
    m_pause_actors.clear();
}

void ThreadActor::send_wrong_state_error(Message const& message, StringView error_message)
{
    JsonObject response;
    response.set("error"sv, "wrongState"sv);
    response.set("message"sv, error_message);
    send_response(message, move(response));
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
