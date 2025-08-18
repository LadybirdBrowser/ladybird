/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_joystick.h>

namespace Web::Internals {

class InternalGamepad : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(InternalGamepad, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(InternalGamepad);

public:
    static GC::Ref<InternalGamepad> create(JS::Realm&);

    virtual ~InternalGamepad() override;

    Array<SDL_GamepadButton, 15> const& buttons();
    Array<SDL_GamepadAxis, 4> const& axes();
    Array<SDL_GamepadAxis, 2> const& triggers();

    void set_button(int button, bool down);
    void set_axis(int axis, short value);

    GC::RootVector<JS::Object*> get_received_rumble_effects() const;
    GC::RootVector<JS::Object*> get_received_rumble_trigger_effects() const;

    void received_rumble(u16 low_frequency_rumble, u16 high_frequency_rumble);
    void received_rumble_triggers(u16 left_rumble, u16 right_rumble);

    void disconnect();

private:
    InternalGamepad(JS::Realm&);
    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;
    virtual void finalize() override;

    SDL_JoystickID m_sdl_joystick_id;
    SDL_Joystick* m_sdl_joystick;
    Vector<GC::Ref<JS::Object>> m_received_rumble_effects;
    Vector<GC::Ref<JS::Object>> m_received_rumble_trigger_effects;
};

}
