/*
 * Copyright (c) 2023-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Interface/LadybirdWebViewBridge.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/Rect.h>
#include <LibIPC/File.h>
#include <LibWebView/Application.h>

#import <Interface/Palette.h>

namespace Ladybird {

template<typename T>
static T scale_for_device(T size, double device_pixel_ratio)
{
    return size.template to_type<double>().scaled(device_pixel_ratio).template to_type<int>();
}

ErrorOr<NonnullOwnPtr<WebViewBridge>> WebViewBridge::create(Vector<Web::DevicePixelRect> screen_rects, double device_pixel_ratio, u64 maximum_frames_per_second)
{
    return adopt_nonnull_own_or_enomem(new (nothrow) WebViewBridge(move(screen_rects), device_pixel_ratio, maximum_frames_per_second));
}

WebViewBridge::WebViewBridge(Vector<Web::DevicePixelRect> screen_rects, double device_pixel_ratio, u64 maximum_frames_per_second)
    : m_screen_rects(move(screen_rects))
{
    m_device_pixel_ratio = device_pixel_ratio;
    m_maximum_frames_per_second = static_cast<double>(maximum_frames_per_second);
}

WebViewBridge::~WebViewBridge() = default;

void WebViewBridge::set_device_pixel_ratio(double device_pixel_ratio)
{
    m_device_pixel_ratio = device_pixel_ratio;
}

void WebViewBridge::set_zoom_level(double zoom_level)
{
    m_zoom_level = zoom_level;
    update_zoom();
}

void WebViewBridge::set_viewport_rect(Gfx::IntRect viewport_rect)
{
    viewport_rect.set_size(scale_for_device(viewport_rect.size(), device_pixel_ratio()));
    m_viewport_size = viewport_rect.size();

    handle_resize();
}

void WebViewBridge::set_maximum_frames_per_second(u64 maximum_frames_per_second)
{
    m_maximum_frames_per_second = static_cast<double>(maximum_frames_per_second);
    client().async_set_maximum_frames_per_second(m_client_state.page_index, maximum_frames_per_second);
}

void WebViewBridge::update_palette()
{
    auto theme = create_system_palette();
    client().async_update_system_theme(m_client_state.page_index, move(theme));
}

void WebViewBridge::enqueue_input_event(Web::MouseEvent event)
{
    event.position = to_content_position(event.position.to_type<int>()).to_type<Web::DevicePixels>();
    event.screen_position = to_content_position(event.screen_position.to_type<int>()).to_type<Web::DevicePixels>();
    ViewImplementation::enqueue_input_event(move(event));
}

void WebViewBridge::enqueue_input_event(Web::DragEvent event)
{
    event.position = to_content_position(event.position.to_type<int>()).to_type<Web::DevicePixels>();
    event.screen_position = to_content_position(event.screen_position.to_type<int>()).to_type<Web::DevicePixels>();
    ViewImplementation::enqueue_input_event(move(event));
}

void WebViewBridge::enqueue_input_event(Web::KeyEvent event)
{
    ViewImplementation::enqueue_input_event(move(event));
}

void WebViewBridge::enqueue_input_event(Web::PinchEvent event)
{
    ViewImplementation::enqueue_input_event(move(event));
}

Optional<WebViewBridge::Paintable> WebViewBridge::paintable()
{
    Gfx::Bitmap const* bitmap = nullptr;
    Gfx::IntSize bitmap_size;
    void* iosurface_ref = nullptr;

    if (m_client_state.has_usable_bitmap) {
        bitmap = m_client_state.front_bitmap.bitmap.ptr();
        bitmap_size = m_client_state.front_bitmap.last_painted_size.to_type<int>();
        iosurface_ref = m_client_state.front_bitmap.iosurface_ref;
    } else {
        bitmap = m_backup_bitmap.ptr();
        bitmap_size = m_backup_bitmap_size.to_type<int>();
    }

    if (!bitmap)
        return {};
    return Paintable { *bitmap, bitmap_size, iosurface_ref };
}

void WebViewBridge::update_zoom()
{
    WebView::ViewImplementation::update_zoom();

    if (on_zoom_level_changed)
        on_zoom_level_changed();
}

Web::DevicePixelSize WebViewBridge::viewport_size() const
{
    return m_viewport_size.to_type<Web::DevicePixels>();
}

Gfx::IntPoint WebViewBridge::to_content_position(Gfx::IntPoint widget_position) const
{
    return scale_for_device(widget_position, device_pixel_ratio());
}

Gfx::IntPoint WebViewBridge::to_widget_position(Gfx::IntPoint content_position) const
{
    return scale_for_device(content_position, inverse_device_pixel_ratio());
}

void WebViewBridge::initialize_client(CreateNewClient create_new_client)
{
    ViewImplementation::initialize_client(create_new_client);
    update_palette();

    if (!m_screen_rects.is_empty()) {
        // FIXME: Update the screens again if they ever change.
        client().async_update_screen_rects(m_client_state.page_index, m_screen_rects, 0);
    }
}

void WebViewBridge::initialize_client_as_child(WebViewBridge& parent, u64 page_index)
{
    m_client_state.client = parent.client();
    m_client_state.page_index = page_index;

    initialize_client(CreateNewClient::No);
}

}
