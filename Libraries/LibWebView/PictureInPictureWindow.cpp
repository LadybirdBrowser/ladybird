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

static Gfx::IntSize size_with_width(Gfx::IntSize aspect_ratio, int width)
{
    return { width, width * aspect_ratio.height() / aspect_ratio.width() };
}

static Gfx::IntSize size_with_height(Gfx::IntSize aspect_ratio, int height)
{
    return { height * aspect_ratio.width() / aspect_ratio.height(), height };
}

// https://w3c.github.io/picture-in-picture/#pip
// It is also RECOMMENDED that the Picture-in-Picture window has a maximum and minimum size. For example, it could be
// restricted to be between a quarter and a half of one dimension of the screen.
// NB: The window opens at a quarter of the screen's width, and can shrink below that for as long as its controls fit.
static constexpr int minimum_width = 240;
static constexpr int minimum_height = 100;

Gfx::IntSize PictureInPictureWindow::minimum_size(Gfx::IntSize video_size)
{
    auto ratio = aspect_ratio(video_size);
    auto size = size_with_width(ratio, minimum_width);
    if (size.height() < minimum_height)
        size = size_with_height(ratio, minimum_height);
    return size;
}

Gfx::IntSize PictureInPictureWindow::maximum_size(Gfx::IntSize video_size, Gfx::IntSize screen_size)
{
    auto ratio = aspect_ratio(video_size);
    auto size = size_with_width(ratio, screen_size.width() / 2);
    if (size.height() > screen_size.height() / 2)
        size = size_with_height(ratio, screen_size.height() / 2);
    return size;
}

Gfx::IntSize PictureInPictureWindow::initial_size(Gfx::IntSize video_size, Gfx::IntSize screen_size)
{
    return size_for_video_size({ screen_size.width() / 4, 0 }, video_size, screen_size);
}

Gfx::IntSize PictureInPictureWindow::size_for_video_size(Gfx::IntSize window_size, Gfx::IntSize video_size, Gfx::IntSize screen_size)
{
    auto minimum = minimum_size(video_size);
    if (window_size.width() <= minimum.width())
        return minimum;

    auto maximum = maximum_size(video_size, screen_size);
    if (window_size.width() >= maximum.width())
        return maximum;

    return size_with_width(aspect_ratio(video_size), window_size.width());
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
