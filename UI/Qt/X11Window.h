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

}
