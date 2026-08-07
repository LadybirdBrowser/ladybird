/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Size.h>
#include <LibWebView/Forward.h>

namespace Compositor {

void paint_paused_debugger_overlay(Gfx::PaintingSurface&, Gfx::IntSize viewport_size, double device_pixel_ratio, Optional<String> const& font_family, Optional<WebView::PausedDebuggerOverlayAction> hovered_action);

}
