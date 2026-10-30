/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Forward.h>

class QWidget;

namespace Ladybird {

// Does nothing unless the widget's window is on an X11 display.
void set_x11_window_aspect_ratio(QWidget&, Gfx::IntSize);

// Whether translucent windows blend with what lies beneath them. Only an X11 display without a compositing manager
// lacks that.
bool windows_blend_with_desktop();

// A translucent window shows black where it is clear, and darkened where it is partly clear, unless it blends with
// what lies beneath it. That needs a compositing manager, and even then only a top-level window blends. From then on,
// each translucent window that cannot blend is cut down to the pixels it paints fully opaque, so a card loses its
// shadow instead of sitting on a black box. Does nothing unless the application is on an X11 display.
void clip_translucent_windows_that_cannot_blend();

// Whether a translucent window has been cut down to the pixels it paints opaque. Its partly clear pixels show darkened
// or not at all, so it paints its edges opaque.
bool is_clipped_to_its_opaque_pixels(QWidget const& window);

}
