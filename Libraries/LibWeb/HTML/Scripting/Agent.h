/*
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/OwnPtr.h>
#include <AK/Vector.h>
#include <LibGC/Root.h>
#include <LibJS/Forward.h>
#include <LibWeb/DOM/MutationObserver.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/CustomElements/CustomElementReactionsStack.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/webappapis.html#similar-origin-window-agent
struct Agent {
    GC::Root<HTML::EventLoop> event_loop;

    // FIXME: These should only be on similar-origin window agents, but we don't currently differentiate agent types.

    // https://dom.spec.whatwg.org/#mutation-observer-compound-microtask-queued-flag
    bool mutation_observer_microtask_queued { false };

    // https://dom.spec.whatwg.org/#mutation-observer-list
    DOM::MutationObserver::List mutation_observers;

    // https://html.spec.whatwg.org/multipage/custom-elements.html#custom-element-reactions-stack
    // Each similar-origin window agent has a custom element reactions stack, which is initially empty.
    CustomElementReactionsStack custom_element_reactions_stack {};

    // https://dom.spec.whatwg.org/#signal-slot-list
    // Each similar-origin window agent has signal slots (a set of slots), which is initially empty. [HTML]
    Vector<GC::Root<HTML::HTMLSlotElement>> signal_slots;

    // https://html.spec.whatwg.org/multipage/custom-elements.html#current-element-queue
    // A similar-origin window agent's current element queue is the element queue at the top of its custom element reactions stack.
    Vector<GC::Root<DOM::Element>>& current_element_queue() { return custom_element_reactions_stack.element_queue_stack.last(); }
    Vector<GC::Root<DOM::Element>> const& current_element_queue() const { return custom_element_reactions_stack.element_queue_stack.last(); }
};

Agent& relevant_agent(JS::Object const&);

}
