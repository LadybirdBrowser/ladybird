/*
 * Copyright (c) 2021-2022, Andreas Kling <kling@serenityos.org>
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
#include <LibWeb/HTML/EventLoop/EventLoop.h>

namespace Web::Bindings {

// https://html.spec.whatwg.org/multipage/custom-elements.html#custom-element-reactions-stack
struct CustomElementReactionsStack {
    CustomElementReactionsStack() = default;
    ~CustomElementReactionsStack() = default;

    // https://html.spec.whatwg.org/multipage/custom-elements.html#element-queue
    // Each item in the stack is an element queue, which is initially empty as well. Each item in an element queue is an element.
    // (The elements are not necessarily custom yet, since this queue is used for upgrades as well.)
    Vector<Vector<JS::Handle<DOM::Element>>> element_queue_stack;

    // https://html.spec.whatwg.org/multipage/custom-elements.html#backup-element-queue
    // Each custom element reactions stack has an associated backup element queue, which an initially-empty element queue.
    Vector<JS::Handle<DOM::Element>> backup_element_queue;

    // https://html.spec.whatwg.org/multipage/custom-elements.html#processing-the-backup-element-queue
    // To prevent reentrancy when processing the backup element queue, each custom element reactions stack also has a processing the backup element queue flag, initially unset.
    bool processing_the_backup_element_queue { false };
};

struct WebEngineCustomData final : public JS::VM::CustomData {
    virtual ~WebEngineCustomData() override = default;

    virtual void spin_event_loop_until(JS::SafeFunction<bool()> goal_condition) override;

    JS::Handle<HTML::EventLoop> event_loop;

    // FIXME: These should only be on similar-origin window agents, but we don't currently differentiate agent types.

    // https://dom.spec.whatwg.org/#mutation-observer-compound-microtask-queued-flag
    bool mutation_observer_microtask_queued { false };

    // https://dom.spec.whatwg.org/#mutation-observer-list
    // FIXME: This should be a set.
    Vector<JS::NonnullGCPtr<DOM::MutationObserver>> mutation_observers;

    JS::Handle<JS::Realm> internal_realm;

    OwnPtr<JS::ExecutionContext> root_execution_context;

    // https://html.spec.whatwg.org/multipage/custom-elements.html#custom-element-reactions-stack
    // Each similar-origin window agent has a custom element reactions stack, which is initially empty.
    CustomElementReactionsStack custom_element_reactions_stack {};

    // https://html.spec.whatwg.org/multipage/custom-elements.html#current-element-queue
    // A similar-origin window agent's current element queue is the element queue at the top of its custom element reactions stack.
    Vector<JS::Handle<DOM::Element>>& current_element_queue() { return custom_element_reactions_stack.element_queue_stack.last(); }
    Vector<JS::Handle<DOM::Element>> const& current_element_queue() const { return custom_element_reactions_stack.element_queue_stack.last(); }
};

struct WebEngineCustomJobCallbackData final : public JS::JobCallback::CustomData {
    WebEngineCustomJobCallbackData(HTML::EnvironmentSettingsObject& incumbent_settings, OwnPtr<JS::ExecutionContext> active_script_context)
        : incumbent_settings(incumbent_settings)
        , active_script_context(move(active_script_context))
    {
    }

    virtual ~WebEngineCustomJobCallbackData() override = default;

    JS::NonnullGCPtr<HTML::EnvironmentSettingsObject> incumbent_settings;
    OwnPtr<JS::ExecutionContext> active_script_context;
};

HTML::Script* active_script();

ErrorOr<void> initialize_main_thread_vm(HTML::EventLoop::Type);
JS::VM& main_thread_vm();

void queue_mutation_observer_microtask(DOM::Document const&);
NonnullOwnPtr<JS::ExecutionContext> create_a_new_javascript_realm(JS::VM&, Function<JS::Object*(JS::Realm&)> create_global_object, Function<JS::Object*(JS::Realm&)> create_global_this_value);
void invoke_custom_element_reactions(Vector<JS::Handle<DOM::Element>>& element_queue);

}
