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

    // NB: The boolean fields of the listener are flags of the cell.

    // capture (a boolean, initially false)
    bool capture() const { return has_flag(Flag::Capture); }
    void set_capture(bool value) { set_flag(Flag::Capture, value); }

    // callback (null or an EventListener object)
    GC::Member<IDLEventListener> callback;

    // signal (null or an AbortSignal object)
    GC::Member<DOM::AbortSignal> signal;

    // type (a string)
    Utf16FlyString type;

    // passive (null or a boolean, initially null)
    Optional<bool> passive() const
    {
        if (!has_flag(Flag::PassiveIsSet))
            return {};
        return has_flag(Flag::Passive);
    }
    void set_passive(Optional<bool> value)
    {
        set_flag(Flag::PassiveIsSet, value.has_value());
        set_flag(Flag::Passive, value.value_or(false));
    }

    // once (a boolean, initially false)
    bool once() const { return has_flag(Flag::Once); }
    void set_once(bool value) { set_flag(Flag::Once, value); }

    // removed (a boolean for bookkeeping purposes, initially false)
    bool removed() const { return has_flag(Flag::Removed); }
    void set_removed(bool value) { set_flag(Flag::Removed, value); }

private:
    virtual void visit_edges(Cell::Visitor&) override;

    enum class Flag : u8 {
        Capture = 1 << 0,
        Once = 1 << 1,
        Removed = 1 << 2,
        PassiveIsSet = 1 << 3,
        Passive = 1 << 4,
    };

    bool has_flag(Flag flag) const { return embedder_flags() & to_underlying(flag); }
    void set_flag(Flag flag, bool value) { set_embedder_flags(static_cast<u8>(value ? (embedder_flags() | to_underlying(flag)) : (embedder_flags() & ~to_underlying(flag)))); }
};

}
