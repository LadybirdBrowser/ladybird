/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024, Glenn Skrzypczak <glenn.skrzypczak@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <LibGC/Ptr.h>
#include <LibJS/Runtime/Object.h>
#include <LibWeb/Forward.h>

namespace Web::DOM {

// https://dom.spec.whatwg.org/#concept-event-listener
// NOTE: The spec calls this "event listener", and it's *importantly* not the same as "EventListener"
class DOMEventListener : public JS::Cell {
    GC_CELL(DOMEventListener, JS::Cell);
    GC_DECLARE_ALLOCATOR(DOMEventListener);

public:
    DOMEventListener();
    ~DOMEventListener();

    // NB: The capture flag and the callback come first, so that they fill the padding at the end of the cell header.
    // capture (a boolean, initially false)
    bool capture { false };

    // callback (null or an EventListener object)
    GC::Member<IDLEventListener> callback;

    // type (a string)
    Utf16FlyString type;

    // signal (null or an AbortSignal object)
    GC::Member<DOM::AbortSignal> signal;

    // passive (null or a boolean, initially null)
    Optional<bool> passive;

    // once (a boolean, initially false)
    bool once { false };

    // removed (a boolean for bookkeeping purposes, initially false)
    bool removed { false };

private:
    virtual void visit_edges(Cell::Visitor&) override;
};

}
