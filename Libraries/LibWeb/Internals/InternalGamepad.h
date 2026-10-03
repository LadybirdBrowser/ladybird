/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/RootVector.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Export.h>
#include <LibWebCommon/Gamepad/GamepadSnapshot.h>

namespace JS {

class Object;

}

namespace Web::Internals {

class InternalGamepad : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(InternalGamepad, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(InternalGamepad);

public:
    static constexpr bool OVERRIDES_FINALIZE = true;

    [[nodiscard]] static GC::Ref<InternalGamepad> create(HTML::Window&, GC::Ref<Internals>, Gamepad::VirtualGamepad);

    virtual ~InternalGamepad() override;

    Vector<i32> const& buttons() const { return m_buttons; }
    Vector<i32> const& axes() const { return m_axes; }
    Vector<i32> const& triggers() const { return m_triggers; }

    void set_button(i32 button, bool down);
    void set_axis(i32 axis, i16 value);

    GC::RootVector<GC::Ref<JS::Object>> get_received_rumble_effects(JS::Object& relevant_global_object) const;
    GC::RootVector<GC::Ref<JS::Object>> get_received_rumble_trigger_effects(JS::Object& relevant_global_object) const;

    void disconnect();

private:
    InternalGamepad(HTML::Window&, GC::Ref<Internals>, Gamepad::VirtualGamepad);
    virtual void visit_edges(GC::Cell::Visitor&) override;
    virtual void finalize() override;

    Page& page() const;

    GC::Ref<HTML::Window> m_window;
    Gamepad::GamepadHandle m_handle { 0 };
    Vector<i32> m_buttons;
    Vector<i32> m_axes;
    Vector<i32> m_triggers;
    bool m_disconnected { false };
    GC::Ref<Internals> m_internals;
};

}
