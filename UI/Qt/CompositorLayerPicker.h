/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <LibGfx/Point.h>

namespace Ladybird {

// What a page tells the compositor layers window of the pointer while the window picks a layer from it, in the device
// pixels of the page's viewport. The page takes no input of the pointer or the keyboard meanwhile, and stops after a
// pick or a cancel.
struct CompositorLayerPicker {
    Function<void(Gfx::IntPoint)> on_hover;
    Function<void()> on_leave;
    Function<void(Gfx::IntPoint)> on_pick;
    Function<void()> on_cancel;
};

}
