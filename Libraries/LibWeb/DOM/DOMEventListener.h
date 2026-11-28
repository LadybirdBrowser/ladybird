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
#include <LibWeb/DOM/RustFFI.h>
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

    // NB: The boolean fields of the listener are flags of the cell, see the EVENT_LISTENER_* flags in
    //     Rust/src/dom/event_listeners.rs, which reads the listener in place.

    // capture (a boolean, initially false)
    bool capture() const { return has_flag(RustFFI::EVENT_LISTENER_CAPTURE); }
    void set_capture(bool value) { set_flag(RustFFI::EVENT_LISTENER_CAPTURE, value); }

    // callback (null or an EventListener object)
    GC::Member<IDLEventListener> callback;

    // signal (null or an AbortSignal object)
    GC::Member<DOM::AbortSignal> signal;

    // type (a string)
    Utf16FlyString type;

    // passive (null or a boolean, initially null)
    Optional<bool> passive() const
    {
        if (!has_flag(RustFFI::EVENT_LISTENER_PASSIVE_IS_SET))
            return {};
        return has_flag(RustFFI::EVENT_LISTENER_PASSIVE);
    }
    void set_passive(Optional<bool> value)
    {
        set_flag(RustFFI::EVENT_LISTENER_PASSIVE_IS_SET, value.has_value());
        set_flag(RustFFI::EVENT_LISTENER_PASSIVE, value.value_or(false));
    }

    // once (a boolean, initially false)
    bool once() const { return has_flag(RustFFI::EVENT_LISTENER_ONCE); }
    void set_once(bool value) { set_flag(RustFFI::EVENT_LISTENER_ONCE, value); }

    // removed (a boolean for bookkeeping purposes, initially false)
    bool removed() const { return has_flag(RustFFI::EVENT_LISTENER_REMOVED); }

private:
    virtual void visit_edges(Cell::Visitor&) override;

    bool has_flag(u8 flag) const { return embedder_flags() & flag; }
    void set_flag(u8 flag, bool value) { set_embedder_flags(static_cast<u8>(value ? (embedder_flags() | flag) : (embedder_flags() & ~flag))); }
};

}
