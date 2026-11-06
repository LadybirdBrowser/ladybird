/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/Optional.h>
#include <AK/Platform.h>
#include <AK/Types.h>
#include <LibGfx/Forward.h>
#include <LibWebView/Forward.h>

#include <Qt>

class QColor;
class QDrag;
class QMargins;
class QWheelEvent;
class QWidget;

namespace Ladybird {

#if defined(AK_OS_MACOS)
// The CGDirectDisplayID of the screen the widget's window is on, or of the main screen while it has no window yet.
Optional<u64> appkit_display_id_for_window(QWidget&);
void hide_appkit_window_title(QWidget&);
void make_appkit_window_resizable(QWidget&);
void keep_appkit_window_visible_while_inactive(QWidget&);
// For windows that animate their own showing and hiding.
void disable_appkit_window_animations(QWidget&);
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
// Rounds the corners of a native child widget's view, which shows what is below it through them, and outlines it.
void round_appkit_view_corners(QWidget&, qreal radius, QColor const& border_color);
// The opacity of a native view embedded in another window, which has no window opacity of its own.
void set_appkit_view_opacity(QWidget&, qreal opacity);
// Clips a native view to rounded bottom corners, for a sheet that hangs from what is above it.
void round_appkit_view_bottom_corners(QWidget&, qreal radius);
// Casts a shadow around a native view. Being outside of the view, the shadow takes no mouse input.
void cast_appkit_view_shadow(QWidget&, qreal corner_radius, qreal blur_radius, qreal y_offset, QColor const&);
// A shadow right below a native view, in a plain view of its own. It is no Qt window, as Qt on macOS takes a child
// window to be under the mouse even when it is transparent for input, and keeps the mouse from what is below it.
class AppKitViewShadow {
    AK_MAKE_NONCOPYABLE(AppKitViewShadow);
    AK_MAKE_NONMOVABLE(AppKitViewShadow);

public:
    AppKitViewShadow();
    ~AppKitViewShadow();

    // The shadow is clipped to the given room around the view.
    void follow(QWidget&, QMargins const& room, qreal corner_radius, qreal blur_radius, qreal y_offset, QColor const& shadow_color, QColor const& fill_color);
    void hide();
    void set_opacity(qreal);

private:
    void* m_view { nullptr };
    void* m_caster { nullptr };
};
bool start_appkit_window_drag(QWidget&);
Qt::DropAction execute_appkit_tab_drag(QWidget&, QDrag&);
void show_appkit_dictionary_lookup(QWidget&, WebView::DictionaryLookup const&, Gfx::IntPoint);
#endif

}
