/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Platform.h>

class QWidget;
class QColor;

namespace Ladybird {

#if defined(AK_OS_MACOS)
void set_rounded_window_corners(QWidget&, bool enabled, double radius, QColor const& background_color);
void install_always_active_window_control_hover_tracking(QWidget&, void (*hover_changed)(QWidget*));
void install_appkit_event_capture();
bool start_appkit_window_drag(QWidget&);
#endif

}
