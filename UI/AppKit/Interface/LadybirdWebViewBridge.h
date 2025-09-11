/*
 * Copyright (c) 2023-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibGfx/Size.h>
#include <LibWeb/Page/InputEvent.h>
#include <LibWebView/ViewImplementation.h>

namespace Ladybird {

class WebViewBridge final : public WebView::ViewImplementation {
public:
    static ErrorOr<NonnullOwnPtr<WebViewBridge>> create(Vector<Web::DevicePixelRect> screen_rects, float device_pixel_ratio, u64 maximum_frames_per_second);
    virtual ~WebViewBridge() override;

    virtual void initialize_client(CreateNewClient = CreateNewClient::Yes) override;
    void initialize_client_as_child(WebViewBridge& parent, u64 page_index);

    float device_pixel_ratio() const { return m_device_pixel_ratio; }
    void set_device_pixel_ratio(float device_pixel_ratio);
    float inverse_device_pixel_ratio() const { return 1.0f / m_device_pixel_ratio; }

    void set_viewport_rect(Gfx::IntRect);

    void set_maximum_frames_per_second(u64 maximum_frames_per_second);

    void update_palette();

    void enqueue_input_event(Web::MouseEvent);
    void enqueue_input_event(Web::DragEvent);
    void enqueue_input_event(Web::KeyEvent);

    struct Paintable {
        Gfx::Bitmap const& bitmap;
        Gfx::IntSize bitmap_size;
    };
    Optional<Paintable> paintable();

    Function<void()> on_zoom_level_changed;

private:
    WebViewBridge(Vector<Web::DevicePixelRect> screen_rects, float device_pixel_ratio, u64 maximum_frames_per_second);

    virtual void update_zoom() override;
    virtual Web::DevicePixelSize viewport_size() const override;
    virtual Gfx::IntPoint to_content_position(Gfx::IntPoint widget_position) const override;
    virtual Gfx::IntPoint to_widget_position(Gfx::IntPoint content_position) const override;

    Vector<Web::DevicePixelRect> m_screen_rects;
    Gfx::IntSize m_viewport_size;
};

}
