/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/RootVector.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Export.h>
#include <LibWeb/Gamepad/SDLGamepadForward.h>

namespace JS {

class Object;
class Realm;

}

namespace Web::Internals {

class InternalGamepad : public Bindings::Wrappable {
    WEB_WRAPPABLE(InternalGamepad, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(InternalGamepad);

public:
    static constexpr bool OVERRIDES_FINALIZE = true;

    [[nodiscard]] static GC::Ref<InternalGamepad> create(GC::Ref<Internals>);

    virtual ~InternalGamepad() override;

    Array<i32, 15> const& buttons();
    Array<i32, 4> const& axes();
    Array<i32, 2> const& triggers();

    void set_button(int button, bool down);
    void set_axis(int axis, short value);

    struct ReceivedRumbleEffect {
        u16 low_frequency_rumble { 0 };
        u16 high_frequency_rumble { 0 };
    };

    struct ReceivedRumbleTriggerEffect {
        u16 left_rumble { 0 };
        u16 right_rumble { 0 };
    };

    Vector<ReceivedRumbleEffect> const& received_rumble_effects() const { return m_received_rumble_effects; }
    Vector<ReceivedRumbleTriggerEffect> const& received_rumble_trigger_effects() const { return m_received_rumble_trigger_effects; }
    GC::RootVector<JS::Object*> get_received_rumble_effects(JS::Realm&) const;
    GC::RootVector<JS::Object*> get_received_rumble_trigger_effects(JS::Realm&) const;

    void received_rumble(u16 low_frequency_rumble, u16 high_frequency_rumble);
    void received_rumble_triggers(u16 left_rumble, u16 right_rumble);

    void disconnect();

private:
    InternalGamepad(GC::Ref<Internals>);
    virtual void visit_edges(GC::Cell::Visitor&) override;
    virtual void finalize() override;

    SDL_JoystickID m_sdl_joystick_id;
    SDL_Joystick* m_sdl_joystick;
    Vector<ReceivedRumbleEffect> m_received_rumble_effects;
    Vector<ReceivedRumbleTriggerEffect> m_received_rumble_trigger_effects;
    GC::Ref<Internals> m_internals;
};

}
