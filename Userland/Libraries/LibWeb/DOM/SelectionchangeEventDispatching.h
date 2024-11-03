/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/EventTarget.h>

namespace Web::DOM {

// https://w3c.github.io/selection-api/#dfn-has-scheduled-selectionchange-event
template<typename T>
concept SelectionChangeTarget = DerivedFrom<T, EventTarget> && requires(T t) {
    { t.has_scheduled_selectionchange_event() } -> SameAs<bool>;
    { t.set_scheduled_selectionchange_event(bool()) } -> SameAs<void>;
};

// https://w3c.github.io/selection-api/#scheduling-selectionhange-event
template<SelectionChangeTarget T>
void schedule_a_selectionchange_event(T& target, Document& document)
{
    // 1. If target's has scheduled selectionchange event is true, abort these steps.
    if (target.has_scheduled_selectionchange_event())
        return;

    // AD-HOC (https://github.com/w3c/selection-api/issues/338):
    // Set target's has scheduled selectionchange event to true
    target.set_scheduled_selectionchange_event(true);

    // 2. Queue a task on the user interaction task source to fire a selectionchange event on
    //    target.
    queue_global_task(HTML::Task::Source::UserInteraction, relevant_global_object(document), JS::create_heap_function(document.heap(), [&] {
        fire_a_selectionchange_event(target, document);
    }));
}

// https://w3c.github.io/selection-api/#firing-selectionhange-event
template<SelectionChangeTarget T>
void fire_a_selectionchange_event(T& target, Document& document)
{
    // 1. Set target's has scheduled selectionchange event to false.
    target.set_scheduled_selectionchange_event(false);

    // 2. If target is an element, fire an event named selectionchange, which bubbles and not
    //    cancelable, at target.
    // 3. Otherwise, if target is a document, fire an event named selectionchange, which does not
    //    bubble and not cancelable, at target.
    EventInit event_init;
    event_init.bubbles = DerivedFrom<T, Element>;
    event_init.cancelable = false;

    auto event = DOM::Event::create(document.realm(), HTML::EventNames::selectionchange, event_init);
    target.dispatch_event(event);
}

}
