/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace Ladybird {

// Whether the Wayland compositor can move a window with the pointer while it is dragged. It never can off Wayland.
bool wayland_compositor_moves_dragged_windows();

}
