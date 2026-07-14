/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Platform.h>
#include <LibGfx/Forward.h>
#include <LibWebView/Forward.h>

class QWidget;
namespace Gfx {

class Color;

}

namespace Ladybird {

#if defined(AK_OS_MACOS)
void hide_appkit_window_title(QWidget&);
void offset_appkit_window_controls(QWidget&, int x_offset, int y_offset);
void install_appkit_event_capture();
void make_appkit_window_first_responder(QWidget&);
bool start_appkit_window_drag(QWidget&);
void show_appkit_dictionary_lookup(QWidget&, WebView::DictionaryLookup const&, Gfx::IntPoint);
Gfx::Color appkit_web_inactive_selection_color();
Gfx::Color appkit_web_inactive_selection_text_color();
#endif

}
