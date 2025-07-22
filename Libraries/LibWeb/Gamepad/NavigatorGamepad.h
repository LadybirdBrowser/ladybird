/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibGC/RootVector.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Gamepad {

class NavigatorGamepadPartial {
public:
    WebIDL::ExceptionOr<GC::RootVector<GC::Ptr<Gamepad>>> get_gamepads();

private:
    virtual ~NavigatorGamepadPartial() = default;

    friend class HTML::Navigator;
};

}
