/*
 * Copyright (c) 2021-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibJS/Runtime/JobCallback.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/MutationObserver.h>
#include <LibWeb/Export.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/Scripting/Agent.h>
#include <LibWebCommon/HTML/CrossOriginIsolationMode.h>
#include <LibWebCommon/HTML/WorkerTypes.h>

namespace Web::Bindings {

class WebEngineCustomJobCallbackData final : public JS::Cell {
    GC_CELL(WebEngineCustomJobCallbackData, JS::Cell);
    GC_DECLARE_ALLOCATOR(WebEngineCustomJobCallbackData);

public:
    virtual ~WebEngineCustomJobCallbackData() override = default;

    GC::Ref<HTML::EnvironmentSettingsObject> incumbent_settings;
    OwnPtr<JS::ExecutionContext> active_script_context;

private:
    WebEngineCustomJobCallbackData(HTML::EnvironmentSettingsObject& incumbent_settings, OwnPtr<JS::ExecutionContext> active_script_context)
        : incumbent_settings(incumbent_settings)
        , active_script_context(move(active_script_context))
    {
    }

    virtual void visit_edges(Cell::Visitor&) override;
};

HTML::Script* active_script();

// Sets up the agent of the current thread, with its VM, heap and event loop. Any thread that has none can host one,
// but a process has one window agent at most. What belongs to the process, such as the plugins, is not set up by this.
WEB_API void initialize_agent(HTML::AgentType);

// Agents that start after this inherit it.
WEB_API void set_collect_garbage_on_every_allocation_in_new_agents(bool);

// The VM of the window agent, from that agent's thread only. Code that a worker can reach has to use JS::VM::the().
WEB_API JS::VM& main_thread_vm();

// Ends the agent of the current thread: releases what it keeps outside its heap, runs the events queued on the thread,
// and destroys its VM and heap. The caller has run its closing steps and given up its roots. None of this ends the
// thread or the process, and the agent's thread has to be the one that calls it. Other agents carry on.
WEB_API void shut_down_agent();

// How many agents other than the window agent are alive in this process, on any thread.
WEB_API size_t worker_agent_count();

WEB_API NonnullOwnPtr<JS::ExecutionContext> create_a_new_javascript_realm(JS::VM&, HTML::CrossOriginIsolationMode agent_cluster_cross_origin_isolation_mode, Function<GC::Ref<JS::Object>(JS::Realm&)> create_global_object, Function<GC::Ref<JS::Object>(JS::Realm&)> create_global_this_value);

// Creates a bare Window-backed realm for tests and tools that do not need a full Document.
WEB_API GC::Ref<JS::Realm> create_a_simple_javascript_realm();

// Creates a principal realm backed by a Page and window environment settings object.
WEB_API GC::Ref<JS::Realm> create_a_principal_javascript_realm();

}
