/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Hub/View.h>

namespace Hub {

View::View(ViewHost& host, InitialState initial_state)
    : ViewImplementation(initial_state.is_private)
    , m_host(&host)
{
    m_device_pixel_ratio = initial_state.device_pixel_ratio;
    m_maximum_frames_per_second = initial_state.maximum_frames_per_second;
    m_display_id = initial_state.display_id;
}

void View::prepare_page_for_navigable(WebView::WebContentPage& page)
{
    ViewImplementation::prepare_page_for_navigable(page);

    update_compositor_display_metadata(page);
}

Core::AnonymousBuffer View::system_theme()
{
    return m_host ? m_host->system_theme() : Core::AnonymousBuffer {};
}

Optional<View::ScreenRects> View::screen_rects() const
{
    return m_host ? m_host->screen_rects() : Optional<ScreenRects> {};
}

void View::update_zoom()
{
    ViewImplementation::update_zoom();
    if (m_host)
        m_host->update_zoom();
}

Web::DevicePixelSize View::viewport_size() const
{
    return m_host ? m_host->viewport_size() : Web::DevicePixelSize {};
}

void View::show_context_menu(WebView::Menu& menu, Gfx::IntPoint widget_position)
{
    if (m_host)
        m_host->show_context_menu(menu, widget_position);
}

void View::did_accept_presented_backing_store(i32 bitmap_id, Gfx::IntRect damage_rect)
{
    if (m_host)
        m_host->did_accept_presented_backing_store(bitmap_id, damage_rect);
}

void View::release_backing_store(i32 bitmap_id)
{
    if (m_host)
        m_host->release_backing_store(bitmap_id);
    else
        ViewImplementation::release_backing_store(bitmap_id);
}

}
