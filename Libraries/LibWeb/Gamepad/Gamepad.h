/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Gamepad.h>
#include <LibWeb/Bindings/RustInterface.h>

namespace Web::Gamepad {

class Gamepad : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(Gamepad, gamepad)
};

}
