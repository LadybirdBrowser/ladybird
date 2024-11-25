/*
 * Copyright (c) 2023, Bastiaan van der Plaat <bastiaan.v.d.plaat@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Gamepad {

class NavigatorGamepadMixin {
public:
    virtual ~NavigatorGamepadMixin() = default;

    WebIDL::ExceptionOr<Vector<GC::Ptr<Gamepad>>> get_gamepads();
    WebIDL::ExceptionOr<void> register_new_gamepad(String const& path);

private:
    WebIDL::ExceptionOr<GC::Ref<Gamepad>> construct_gamepad(AK::NonnullRefPtr<Core::Gamepad> underlaying_gamepad);
    int select_unused_gamepad_index();

    bool m_loaded_initial_gamepads { false };
    Vector<GC::Ptr<Gamepad>> m_gamepads;

    friend class Navigator;
};

}
