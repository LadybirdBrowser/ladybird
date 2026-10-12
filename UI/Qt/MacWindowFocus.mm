/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/MacWindow.h>

#include <QGuiApplication>
#include <QWidget>

#import <AppKit/AppKit.h>

namespace Ladybird {

void make_appkit_window_first_responder(QWidget& widget)
{
    if (QGuiApplication::platformName() != "cocoa")
        return;
    auto* window_widget = widget.window();
    if (!window_widget)
        return;

    auto* view = reinterpret_cast<NSView*>(window_widget->winId());
    if (!view)
        return;

    auto* window = view.window;
    if (!window)
        return;

    [window makeFirstResponder:view];
}

}
