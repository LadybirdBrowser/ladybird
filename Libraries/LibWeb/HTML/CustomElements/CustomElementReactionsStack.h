/*
 * Copyright (c) 2021-2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibGC/Root.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/custom-elements.html#custom-element-reactions-stack
struct CustomElementReactionsStack {
    CustomElementReactionsStack() = default;
    ~CustomElementReactionsStack() = default;

    // https://html.spec.whatwg.org/multipage/custom-elements.html#element-queue
    // Each item in the stack is an element queue, which is initially empty as well. Each item in an element queue is an element.
    // (The elements are not necessarily custom yet, since this queue is used for upgrades as well.)
    Vector<Vector<GC::Root<DOM::Element>>> element_queue_stack;

    // https://html.spec.whatwg.org/multipage/custom-elements.html#backup-element-queue
    // Each custom element reactions stack has an associated backup element queue, which an initially-empty element queue.
    Vector<GC::Root<DOM::Element>> backup_element_queue;

    // https://html.spec.whatwg.org/multipage/custom-elements.html#processing-the-backup-element-queue
    // To prevent reentrancy when processing the backup element queue, each custom element reactions stack also has a processing the backup element queue flag, initially unset.
    bool processing_the_backup_element_queue { false };
};

}
