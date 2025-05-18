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
#include <LibWeb/Bindings/AgentType.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/MutationObserver.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/Scripting/Agent.h>

namespace Web::Bindings {

struct WebEngineCustomJobCallbackData final : public JS::JobCallback::CustomData {
    WebEngineCustomJobCallbackData(JS::Realm& incumbent_realm, OwnPtr<JS::ExecutionContext> active_script_context)
        : incumbent_realm(incumbent_realm)
        , active_script_context(move(active_script_context))
    {
    }

    virtual ~WebEngineCustomJobCallbackData() override = default;

    GC::Ref<JS::Realm> incumbent_realm;
    OwnPtr<JS::ExecutionContext> active_script_context;
};

HTML::Script* active_script();

void initialize_main_thread_vm(AgentType);
JS::VM& main_thread_vm();

void queue_mutation_observer_microtask(DOM::Document const&);
NonnullOwnPtr<JS::ExecutionContext> create_a_new_javascript_realm(JS::VM&, Function<JS::Object*(JS::Realm&)> create_global_object, Function<JS::Object*(JS::Realm&)> create_global_this_value);
void invoke_custom_element_reactions(Vector<GC::Root<DOM::Element>>& element_queue);

}
