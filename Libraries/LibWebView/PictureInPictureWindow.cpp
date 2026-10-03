/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/PictureInPictureWindow.h>
#include <LibWebView/ViewImplementation.h>

namespace WebView {

Gfx::IntSize PictureInPictureWindow::aspect_ratio(Gfx::IntSize video_size)
{
    if (video_size.is_empty())
        return { 16, 9 };
    return video_size;
}

static Gfx::IntSize size_with_width(Gfx::IntSize video_size, int width, Gfx::IntSize screen_size)
{
    auto aspect_ratio = PictureInPictureWindow::aspect_ratio(video_size);

    // https://w3c.github.io/picture-in-picture/#pip
    // It is also RECOMMENDED that the Picture-in-Picture window has a maximum and minimum size. For example, it could
    // be restricted to be between a quarter and a half of one dimension of the screen.
    auto height = width * aspect_ratio.height() / aspect_ratio.width();

    if (auto maximum_height = screen_size.height() / 2; height > maximum_height) {
        height = maximum_height;
        width = height * aspect_ratio.width() / aspect_ratio.height();
    }

    return { width, height };
}

Gfx::IntSize PictureInPictureWindow::initial_size(Gfx::IntSize video_size, Gfx::IntSize screen_size)
{
    return size_with_width(video_size, screen_size.width() / 4, screen_size);
}

Gfx::IntSize PictureInPictureWindow::size_for_video_size(Gfx::IntSize window_size, Gfx::IntSize video_size, Gfx::IntSize screen_size)
{
    return size_with_width(video_size, window_size.width(), screen_size);
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
