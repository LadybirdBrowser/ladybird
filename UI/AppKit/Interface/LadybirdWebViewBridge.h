/*
 * Copyright (c) 2023-2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Vector.h>
#include <AK/kmalloc.h>
#include <LibCompositing/InputEvent.h>
#include <LibCompositing/PageId.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibGfx/Size.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/ViewImplementation.h>

namespace Ladybird {

class WebViewBridge final : public WebView::ViewImplementation {
public:
    AK_ALLOC_WITH_KMALLOC;

    static ErrorOr<NonnullOwnPtr<WebViewBridge>> create(WebView::IsPrivate, Vector<Compositing::DevicePixelRect> screen_rects, double device_pixel_ratio, u64 maximum_frames_per_second, Optional<u64> display_id);
    virtual ~WebViewBridge() override;

    using ViewImplementation::initialize_client;
    virtual void prepare_page_for_tab(WebView::WebContentPage&) override;
    void initialize_client_as_child(WebView::WebContentClient& page_process, Compositing::PageId page_index);

    void set_device_pixel_ratio(double device_pixel_ratio);
    void set_zoom_level(double zoom_level);
    double inverse_device_pixel_ratio() const { return 1.0 / device_pixel_ratio(); }

    void set_viewport_rect(Gfx::IntRect);

    void set_display_metadata(u64 maximum_frames_per_second, Optional<u64> display_id);

    void exit_fullscreen();

    void update_palette();
    void update_palette(WebView::WebContentPage&);

    void enqueue_input_event(Compositing::MouseEvent);
    void enqueue_input_event(Web::DragEvent);
    void enqueue_input_event(Compositing::KeyEvent);
    void enqueue_input_event(Compositing::PinchEvent);

    struct Paintable {
        Gfx::SharedImageBuffer const* shared_image_buffer { nullptr };
        Gfx::IntSize bitmap_size;
    };
    Optional<Paintable> paintable();

    Function<void()> on_zoom_level_changed;

private:
    WebViewBridge(WebView::IsPrivate, Vector<Compositing::DevicePixelRect> screen_rects, double device_pixel_ratio, u64 maximum_frames_per_second, Optional<u64> display_id);

    void update_compositor_display_metadata();
    void update_compositor_display_metadata(WebView::WebContentPage&);

    virtual void update_zoom() override;
    virtual Compositing::DevicePixelSize viewport_size() const override;
    virtual Gfx::IntPoint to_content_position(Gfx::IntPoint widget_position) const override;
    virtual Gfx::IntPoint to_widget_position(Gfx::IntPoint content_position) const override;

    Vector<Compositing::DevicePixelRect> m_screen_rects;
    Gfx::IntSize m_viewport_size;
};

}
