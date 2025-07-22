/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::Gamepad {

// https://w3c.github.io/gamepad/#dom-gamepad
class Gamepad : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(Gamepad, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(Gamepad);

private:
    explicit Gamepad(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
};

}
