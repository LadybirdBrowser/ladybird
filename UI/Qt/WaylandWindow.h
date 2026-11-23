/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

class QWindow;
struct wl_surface;

namespace QNativeInterface {

struct QWaylandApplication;

}

namespace Ladybird {

QNativeInterface::QWaylandApplication* wayland_application();

// The surface of an existing native Wayland window. This does not create a platform window.
wl_surface* wayland_surface_for_window(QWindow const&);

// Whether the Wayland compositor can move a window with the pointer while it is dragged. It never can off Wayland.
bool wayland_compositor_moves_dragged_windows();

}
