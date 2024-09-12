/*
 * Copyright (c) 2024, Undefine <undefine@undefine.pl>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Gamepad {

class GamepadButton final : public Web::Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(GamepadButton, Web::Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(GamepadButton);

public:
    static WebIDL::ExceptionOr<GC::Ref<GamepadButton>> create(JS::Realm&, bool pressed);
    virtual ~GamepadButton() override;

    bool pressed() const { return m_pressed; }
    bool touched() const { return m_pressed; }
    double value() const { return m_pressed ? 1.0 : 0.0; }

private:
    GamepadButton(JS::Realm&, bool pressed);

    virtual void initialize(JS::Realm&) override;

    bool m_pressed;
};

}
