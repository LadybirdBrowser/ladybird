/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebView/ViewImplementation.h>

namespace Hub {

// What a view needs from the UI that shows it.
class ViewHost {
public:
    virtual ~ViewHost() = default;

    virtual Core::AnonymousBuffer system_theme() = 0;
    virtual Optional<WebView::ViewImplementation::ScreenRects> screen_rects() const = 0;
    virtual Web::DevicePixelSize viewport_size() const = 0;
    virtual void update_zoom() = 0;
    virtual void show_context_menu(WebView::Menu&, Gfx::IntPoint widget_position) = 0;
    virtual void did_accept_presented_backing_store(i32 bitmap_id, Gfx::IntRect damage_rect) = 0;
    virtual void release_backing_store(i32 bitmap_id) = 0;
};

// A tab's view. It holds the tab's pages and everything else about them, and leaves showing them to its ViewHost.
class View final : public WebView::ViewImplementation {
public:
    AK_ALLOC_WITH_KMALLOC;

    struct InitialState {
        WebView::IsPrivate is_private { WebView::IsPrivate::No };
        double device_pixel_ratio { 1.0 };
        double maximum_frames_per_second { 60.0 };
        Optional<u64> display_id;
    };

    View(ViewHost&, InitialState);

    ViewHost* host() const { return m_host; }

    // The host is going away, and takes no more calls.
    void detach_from_host() { m_host = nullptr; }

    void set_device_pixel_ratio(double device_pixel_ratio) { m_device_pixel_ratio = device_pixel_ratio; }
    bool scales_frames_to_fit() const { return m_scales_frames_to_fit; }
    void release_backing_store_to_page(i32 bitmap_id) { ViewImplementation::release_backing_store(bitmap_id); }

    using ViewImplementation::handle_resize;
    using ViewImplementation::initialize_tab;
    using ViewImplementation::microphone_authorizer;
    using ViewImplementation::rect_fitting_frame;
    using ViewImplementation::set_owner_view;
    using ViewImplementation::set_page_background_color_to_system_canvas;

private:
    // ^WebView::ViewImplementation
    virtual void prepare_page_for_navigable(WebView::WebContentPage&) override;
    virtual Core::AnonymousBuffer system_theme() override;
    virtual Optional<ScreenRects> screen_rects() const override;
    virtual void update_zoom() override;
    virtual Web::DevicePixelSize viewport_size() const override;
    virtual Gfx::IntPoint to_content_position(Gfx::IntPoint widget_position) const override { return widget_position; }
    virtual Gfx::IntPoint to_widget_position(Gfx::IntPoint content_position) const override { return content_position; }
    virtual void show_context_menu(WebView::Menu&, Gfx::IntPoint widget_position) override;
    virtual void did_accept_presented_backing_store(i32 bitmap_id, Gfx::IntRect damage_rect) override;
    virtual void release_backing_store(i32 bitmap_id) override;

    ViewHost* m_host { nullptr };
};

}
