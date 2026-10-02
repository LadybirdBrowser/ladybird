/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/PictureInPictureWindow.h>
#include <LibWebView/ViewImplementation.h>

namespace WebView {

Gfx::IntSize PictureInPictureWindow::initial_size(Gfx::IntSize video_size, Gfx::IntSize screen_size)
{
    if (video_size.is_empty())
        video_size = { 16, 9 };

    // https://w3c.github.io/picture-in-picture/#pip
    // It is also RECOMMENDED that the Picture-in-Picture window has a maximum and minimum size. For example, it could
    // be restricted to be between a quarter and a half of one dimension of the screen.
    auto width = screen_size.width() / 4;
    auto height = width * video_size.height() / video_size.width();

    if (auto maximum_height = screen_size.height() / 2; height > maximum_height) {
        height = maximum_height;
        width = height * video_size.width() / video_size.height();
    }

    return { width, height };
}

void PictureInPictureWindow::report_page_close_of(ViewImplementation& view)
{
    view.on_close = [this] {
        if (on_page_close)
            on_page_close();
    };
    view.on_web_content_crashed = [this](auto) {
        if (on_page_close)
            on_page_close();
    };
}

}
