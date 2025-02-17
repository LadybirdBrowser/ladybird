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
#include <LibWeb/CSS/PreferredColorScheme.h>
#include <LibWeb/CSS/PreferredContrast.h>
#include <LibWeb/CSS/PreferredMotion.h>
#include <LibWeb/Page/InputEvent.h>
#include <LibWebView/ViewImplementation.h>

namespace Ladybird {

class WebViewBridge final : public WebView::ViewImplementation {
public:
    static ErrorOr<NonnullOwnPtr<WebViewBridge>> create(Vector<Web::DevicePixelRect> screen_rects, float device_pixel_ratio, Web::CSS::PreferredColorScheme, Web::CSS::PreferredContrast, Web::CSS::PreferredMotion);
    virtual ~WebViewBridge() override;

    virtual void initialize_client(CreateNewClient = CreateNewClient::Yes) override;
    void initialize_client_as_child(WebViewBridge const& parent, u64 page_index);

    float device_pixel_ratio() const { return m_device_pixel_ratio; }
    void set_device_pixel_ratio(float device_pixel_ratio);
    float inverse_device_pixel_ratio() const { return 1.0f / m_device_pixel_ratio; }

    void set_viewport_rect(Gfx::IntRect);

    void update_palette();
    void set_preferred_color_scheme(Web::CSS::PreferredColorScheme);
    void set_preferred_contrast(Web::CSS::PreferredContrast);
    void set_preferred_motion(Web::CSS::PreferredMotion);

    void enqueue_input_event(Web::MouseEvent);
    void enqueue_input_event(Web::DragEvent);
    void enqueue_input_event(Web::KeyEvent);

    void set_enable_autoplay(bool enabled);

    struct Paintable {
        Gfx::Bitmap& bitmap;
        Gfx::IntSize bitmap_size;
    };
    Optional<Paintable> paintable();

    Function<void()> on_zoom_level_changed;

private:
    WebViewBridge(Vector<Web::DevicePixelRect> screen_rects, float device_pixel_ratio, Web::CSS::PreferredColorScheme, Web::CSS::PreferredContrast, Web::CSS::PreferredMotion);

    virtual void update_zoom() override;
    virtual Web::DevicePixelSize viewport_size() const override;
    virtual Gfx::IntPoint to_content_position(Gfx::IntPoint widget_position) const override;
    virtual Gfx::IntPoint to_widget_position(Gfx::IntPoint content_position) const override;

    Vector<Web::DevicePixelRect> m_screen_rects;
    Gfx::IntSize m_viewport_size;

    Web::CSS::PreferredColorScheme m_preferred_color_scheme { Web::CSS::PreferredColorScheme::Auto };
    Web::CSS::PreferredContrast m_preferred_contrast { Web::CSS::PreferredContrast::Auto };
    Web::CSS::PreferredMotion m_preferred_motion { Web::CSS::PreferredMotion::Auto };
};

}
