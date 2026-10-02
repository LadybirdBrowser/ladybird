/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/String.h>
#include <AK/kmalloc.h>
#include <LibGfx/Size.h>
#include <LibWebView/Forward.h>

namespace WebView {

// The floating window that shows a page's Picture-in-Picture video. Sizes are in CSS pixels.
class WEBVIEW_API PictureInPictureWindow {
public:
    AK_ALLOC_WITH_KMALLOC;

    static Gfx::IntSize initial_size(Gfx::IntSize video_size, Gfx::IntSize screen_size);

    virtual ~PictureInPictureWindow() = default;

    virtual Gfx::IntSize size() const = 0;
    virtual String handle() const = 0;
    virtual void hide() = 0;

    Function<void(Gfx::IntSize)> on_resize;
    Function<void()> on_close;
    Function<void()> on_page_close;

protected:
    void report_page_close_of(ViewImplementation&);
};

}
