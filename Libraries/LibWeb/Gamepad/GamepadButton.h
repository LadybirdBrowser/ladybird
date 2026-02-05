/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::Gamepad {

class GamepadButton final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(GamepadButton, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(GamepadButton);

public:
    virtual ~GamepadButton() override;

    bool pressed() const { return m_pressed; }
    bool touched() const { return m_touched; }
    double value() const { return m_value; }

    void set_pressed(Badge<Gamepad>, bool);
    void set_touched(Badge<Gamepad>, bool);
    void set_value(Badge<Gamepad>, double);

private:
    GamepadButton(JS::Realm&);

    virtual void initialize(JS::Realm&) override;

    // https://w3c.github.io/gamepad/#dfn-pressed
    // A flag indicating that the button is pressed
    bool m_pressed { false };

    // https://w3c.github.io/gamepad/#dfn-touched
    // A flag indicating that the button is touched
    bool m_touched { false };

    // https://w3c.github.io/gamepad/#dfn-value
    // A double representing the button value scaled to the range [0 .. 1]
    double m_value { 0.0 };
};

}
