/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Platform.h>
#include <AK/Types.h>
#include <LibGfx/Forward.h>
#include <LibWebView/Forward.h>

#include <Qt>

class QDrag;
class QWheelEvent;
class QWidget;

namespace Ladybird {

#if defined(AK_OS_MACOS)
// The CGDirectDisplayID of the screen the widget's window is on, or of the main screen while it has no window yet.
Optional<u64> appkit_display_id_for_window(QWidget&);
void hide_appkit_window_title(QWidget&);
void make_appkit_window_resizable(QWidget&);
void keep_appkit_window_visible_while_inactive(QWidget&);
void ensure_appkit_window_is_exposed(QWidget&);
void set_appkit_window_content_aspect_ratio(QWidget&, Gfx::IntSize);
void offset_appkit_window_controls(QWidget&, int x_offset, int y_offset);
// AppKit puts the window controls back in their default place when the window title changes; this moves them back
// to their offset right away, before the next frame is drawn.
void reapply_appkit_window_control_offsets(QWidget&);
void install_appkit_event_capture();
// What AppKit says of a flick's momentum at the scroll event a wheel event came from, which Qt does not pass on.
enum class AppKitScrollMomentumTransition {
    // The fingers lifted, and the momentum of the flick they made is about to begin.
    WillBegin,
    Began,
};
Optional<AppKitScrollMomentumTransition> take_appkit_scroll_momentum_transition_for(QWheelEvent const&);
void make_appkit_window_first_responder(QWidget&);
bool start_appkit_window_drag(QWidget&);
Qt::DropAction execute_appkit_tab_drag(QWidget&, QDrag&);
void show_appkit_dictionary_lookup(QWidget&, WebView::DictionaryLookup const&, Gfx::IntPoint);
#endif

}
