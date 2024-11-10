/*
 * Copyright (c) 2023-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Interface/LadybirdWebViewBridge.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/Rect.h>
#include <LibIPC/File.h>
#include <LibWeb/Crypto/Crypto.h>
#include <LibWebView/Application.h>
#include <LibWebView/UserAgent.h>

#import <Interface/Palette.h>

namespace Ladybird {

template<typename T>
static T scale_for_device(T size, float device_pixel_ratio)
{
    return size.template to_type<float>().scaled(device_pixel_ratio).template to_type<int>();
}

ErrorOr<NonnullOwnPtr<WebViewBridge>> WebViewBridge::create(Vector<Web::DevicePixelRect> screen_rects, float device_pixel_ratio, Web::CSS::PreferredColorScheme preferred_color_scheme, Web::CSS::PreferredContrast preferred_contrast, Web::CSS::PreferredMotion preferred_motion)
{
    return adopt_nonnull_own_or_enomem(new (nothrow) WebViewBridge(move(screen_rects), device_pixel_ratio, preferred_color_scheme, preferred_contrast, preferred_motion));
}

WebViewBridge::WebViewBridge(Vector<Web::DevicePixelRect> screen_rects, float device_pixel_ratio, Web::CSS::PreferredColorScheme preferred_color_scheme, Web::CSS::PreferredContrast preferred_contrast, Web::CSS::PreferredMotion preferred_motion)
    : m_screen_rects(move(screen_rects))
    , m_preferred_color_scheme(preferred_color_scheme)
    , m_preferred_contrast(preferred_contrast)
    , m_preferred_motion(preferred_motion)
{
    m_device_pixel_ratio = device_pixel_ratio;
}

WebViewBridge::~WebViewBridge() = default;

void WebViewBridge::set_device_pixel_ratio(float device_pixel_ratio)
{
    m_device_pixel_ratio = device_pixel_ratio;
    client().async_set_device_pixels_per_css_pixel(m_client_state.page_index, m_device_pixel_ratio * m_zoom_level);
}

void WebViewBridge::set_system_visibility_state(bool is_visible)
{
    client().async_set_system_visibility_state(m_client_state.page_index, is_visible);
}

void WebViewBridge::set_viewport_rect(Gfx::IntRect viewport_rect)
{
    viewport_rect.set_size(scale_for_device(viewport_rect.size(), m_device_pixel_ratio));
    m_viewport_size = viewport_rect.size();

    handle_resize();
}

void WebViewBridge::update_palette()
{
    auto theme = create_system_palette();
    client().async_update_system_theme(m_client_state.page_index, move(theme));
}

void WebViewBridge::set_preferred_color_scheme(Web::CSS::PreferredColorScheme color_scheme)
{
    m_preferred_color_scheme = color_scheme;
    client().async_set_preferred_color_scheme(m_client_state.page_index, color_scheme);
}

void WebViewBridge::set_preferred_contrast(Web::CSS::PreferredContrast contrast)
{
    m_preferred_contrast = contrast;
    client().async_set_preferred_contrast(m_client_state.page_index, contrast);
}

void WebViewBridge::set_preferred_motion(Web::CSS::PreferredMotion motion)
{
    m_preferred_motion = motion;
    client().async_set_preferred_motion(m_client_state.page_index, motion);
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

void WebViewBridge::set_enable_autoplay(bool enabled)
{
    ViewImplementation::set_enable_autoplay(enabled);
}

Optional<WebViewBridge::Paintable> WebViewBridge::paintable()
{
    Gfx::Bitmap* bitmap = nullptr;
    Gfx::IntSize bitmap_size;

    if (m_client_state.has_usable_bitmap) {
        bitmap = m_client_state.front_bitmap.bitmap.ptr();
        bitmap_size = m_client_state.front_bitmap.last_painted_size.to_type<int>();
    } else {
        bitmap = m_backup_bitmap.ptr();
        bitmap_size = m_backup_bitmap_size.to_type<int>();
    }

    if (!bitmap)
        return {};
    return Paintable { *bitmap, bitmap_size };
}

void WebViewBridge::update_zoom()
{
    client().async_set_device_pixels_per_css_pixel(m_client_state.page_index, m_device_pixel_ratio * m_zoom_level);

    if (on_zoom_level_changed)
        on_zoom_level_changed();
}

Web::DevicePixelSize WebViewBridge::viewport_size() const
{
    return m_viewport_size.to_type<Web::DevicePixels>();
}

Gfx::IntPoint WebViewBridge::to_content_position(Gfx::IntPoint widget_position) const
{
    return scale_for_device(widget_position, m_device_pixel_ratio);
}

Gfx::IntPoint WebViewBridge::to_widget_position(Gfx::IntPoint content_position) const
{
    return scale_for_device(content_position, inverse_device_pixel_ratio());
}

void WebViewBridge::initialize_client(CreateNewClient create_new_client)
{
    VERIFY(on_request_web_content);

    if (create_new_client == CreateNewClient::Yes) {
        m_client_state = {};
        m_client_state.client = on_request_web_content();
    } else {
        m_client_state.client->register_view(m_client_state.page_index, *this);
    }

    m_client_state.client->on_web_content_process_crash = [this] {
        Core::deferred_invoke([this] {
            handle_web_content_process_crash();
        });
    };

    m_client_state.client_handle = MUST(Web::Crypto::generate_random_uuid());
    client().async_set_window_handle(m_client_state.page_index, m_client_state.client_handle);

    client().async_set_device_pixels_per_css_pixel(m_client_state.page_index, m_device_pixel_ratio);
    client().async_set_preferred_color_scheme(m_client_state.page_index, m_preferred_color_scheme);
    update_palette();

    if (!m_screen_rects.is_empty()) {
        // FIXME: Update the screens again if they ever change.
        client().async_update_screen_rects(m_client_state.page_index, m_screen_rects, 0);
    }

    if (auto const& webdriver_content_ipc_path = WebView::Application::chrome_options().webdriver_content_ipc_path; webdriver_content_ipc_path.has_value()) {
        client().async_connect_to_webdriver(m_client_state.page_index, *webdriver_content_ipc_path);
    }

    if (auto const& user_agent_preset = WebView::Application::web_content_options().user_agent_preset; user_agent_preset.has_value()) {
        auto user_agent = *WebView::user_agents.get(*user_agent_preset);
        client().async_debug_request(m_client_state.page_index, "spoof-user-agent"sv, user_agent);
    }
}

void WebViewBridge::initialize_client_as_child(WebViewBridge const& parent, u64 page_index)
{
    m_client_state.client = parent.client();
    m_client_state.page_index = page_index;

    initialize_client(CreateNewClient::No);
}

}
