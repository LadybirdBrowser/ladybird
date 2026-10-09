/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Size.h>
#include <UI/Qt/X11Window.h>

#include <QGuiApplication>
#include <QWidget>

#include <xcb/xcb_icccm.h>

namespace Ladybird {

void set_x11_window_aspect_ratio(QWidget& widget, Gfx::IntSize aspect_ratio)
{
    auto* x11_application = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
    if (!x11_application)
        return;

    auto* connection = x11_application->connection();
    auto window = static_cast<xcb_window_t>(widget.winId());

    // Qt writes all of the window's size hints whenever it changes the window's geometry, so the aspect ratio joins the
    // hints it last wrote, and has to be set again after any such change.
    xcb_size_hints_t hints {};
    xcb_icccm_get_wm_normal_hints_reply(connection, xcb_icccm_get_wm_normal_hints(connection, window), &hints, nullptr);
    xcb_icccm_size_hints_set_aspect(&hints, aspect_ratio.width(), aspect_ratio.height(), aspect_ratio.width(), aspect_ratio.height());
    xcb_icccm_set_wm_normal_hints(connection, window, &hints);
    xcb_flush(connection);
}

}
