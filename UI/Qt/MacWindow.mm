/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/MacWindow.h>

#include <QWidget>

#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>

namespace Ladybird {

void set_rounded_window_corners(QWidget& widget, bool enabled, double radius)
{
    auto* view = reinterpret_cast<NSView*>(widget.winId());
    if (!view)
        return;

    auto* window = view.window;
    if (!window)
        return;

    auto* content_view = window.contentView;
    if (!content_view)
        return;

    content_view.wantsLayer = YES;
    content_view.layer.cornerRadius = enabled ? radius : 0.0;
    content_view.layer.masksToBounds = enabled ? YES : NO;
}

}
