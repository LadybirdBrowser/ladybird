/*
 * Copyright (c) 2026, Johan Dahlin <jdahlin@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/Resource.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Palette.h>
#include <LibGfx/SystemTheme.h>
#include <LibWebView/Application.h>
#include <LibWebView/Menu.h>
#include <LibWebView/WebContentClient.h>
#include <UI/Gtk/Events.h>
#include <UI/Gtk/GLibPtr.h>
#include <UI/Gtk/WebContentView.h>

#include <adwaita.h>
#include <gdk/gdk.h>

namespace Ladybird {

WebContentView::WebContentView(LadybirdWebView* widget, RefPtr<WebView::WebContentClient> parent_client, size_t page_index)
    : m_widget(widget)
{
    m_client_state.client = parent_client;
    m_client_state.page_index = page_index;

    m_device_pixel_ratio = gtk_widget_get_scale_factor(GTK_WIDGET(widget));

    // Store ourselves in the GObject widget
    ladybird_web_view_set_impl(widget, this);

    initialize_client((parent_client == nullptr) ? CreateNewClient::Yes : CreateNewClient::No);

    on_ready_to_paint = [this]() {
        if (m_widget)
            gtk_widget_queue_draw(GTK_WIDGET(m_widget));
    };

    on_finish_handling_key_event = [this](Web::KeyEvent const& event) {
        finish_handling_key_event(event);
    };

    // Re-send palette when system theme changes (light <-> dark)
    g_signal_connect_swapped(adw_style_manager_get_default(), "notify::dark", G_CALLBACK(+[](WebContentView* self, GParamSpec*) {
        self->update_palette();
        if (self->m_widget)
            gtk_widget_queue_draw(GTK_WIDGET(self->m_widget));
    }),
        this);
}

WebContentView::~WebContentView()
{
    g_signal_handlers_disconnect_by_data(adw_style_manager_get_default(), this);
    if (m_widget)
        ladybird_web_view_set_impl(m_widget, nullptr);
}

void WebContentView::enqueue_native_event(Web::MouseEvent::Type type, double x, double y, unsigned button, GdkModifierType state, int click_count)
{
    auto device_pixel_ratio = m_device_pixel_ratio;
    auto position = Web::DevicePixelPoint { static_cast<int>(x * device_pixel_ratio), static_cast<int>(y * device_pixel_ratio) };
    auto web_button = gdk_button_to_web(button);
    auto modifiers = gdk_modifier_to_web(state);

    Web::UIEvents::MouseButton buttons;
    switch (type) {
    case Web::MouseEvent::Type::MouseDown:
        buttons = web_button;
        break;
    case Web::MouseEvent::Type::MouseMove:
        buttons = gdk_buttons_to_web(state);
        break;
    default:
        buttons = Web::UIEvents::MouseButton::None;
        break;
    }

    Web::MouseEvent event {
        .type = type,
        .position = position,
        .screen_position = position,
        .button = web_button,
        .buttons = buttons,
        .modifiers = modifiers,
        .wheel_delta_x = 0,
        .wheel_delta_y = 0,
        .click_count = click_count,
        .browser_data = {},
    };
    enqueue_input_event(move(event));
}

void WebContentView::enqueue_native_event(Web::KeyEvent::Type type, guint keyval, GdkModifierType state)
{
    Web::KeyEvent event {
        .type = type,
        .key = gdk_keyval_to_web(keyval),
        .modifiers = gdk_modifier_to_web(state),
        .code_point = gdk_keyval_to_unicode(keyval),
        .repeat = false,
        .browser_data = {},
    };
    enqueue_input_event(move(event));
}

void WebContentView::finish_handling_key_event(Web::KeyEvent const& event)
{
    // FIXME: Re-dispatch the original event through GTK's event system so that
    //        unhandled keys propagate to accelerators/shortcuts generically,
    //        matching the Qt UI's approach of re-sending the original QKeyEvent.
    if (event.type != Web::KeyEvent::Type::KeyDown)
        return;
    if (!(event.modifiers & Web::UIEvents::KeyModifier::Mod_Ctrl))
        return;

    auto& app = WebView::Application::the();
    switch (event.key) {
    case Web::UIEvents::Key_C:
        app.copy_selection_action().activate();
        break;
    case Web::UIEvents::Key_V:
        app.paste_action().activate();
        break;
    case Web::UIEvents::Key_A:
        app.select_all_action().activate();
        break;
    default:
        break;
    }
}

void WebContentView::paint(GtkSnapshot* snapshot)
{
    auto width = gtk_widget_get_width(GTK_WIDGET(m_widget));
    auto height = gtk_widget_get_height(GTK_WIDGET(m_widget));

    if (width == 0 || height == 0)
        return;

    Gfx::Bitmap const* bitmap = nullptr;
    Gfx::IntSize bitmap_size;

    if (m_client_state.has_usable_bitmap) {
        VERIFY(m_client_state.front_bitmap.shared_image_buffer);
        bitmap = m_client_state.front_bitmap.shared_image_buffer->bitmap().ptr();
        bitmap_size = m_client_state.front_bitmap.last_painted_size.to_type<int>();
    } else if (m_backup_shared_image_buffer) {
        bitmap = m_backup_shared_image_buffer->bitmap().ptr();
        bitmap_size = m_backup_bitmap_size.to_type<int>();
    }

    if (bitmap) {
        auto painted_width = bitmap_size.width();
        auto painted_height = bitmap_size.height();
        if (painted_width == 0 || painted_height == 0)
            painted_width = bitmap->width(), painted_height = bitmap->height();

        // Rebuild the GdkTexture when the bitmap data or size changes.
        // Use GdkMemoryTextureBuilder so we can set update-texture to hint GSK
        // that this is an incremental update of the previous frame, allowing it
        // to reuse GPU resources and only re-upload changed regions.
        if (bitmap != m_cached_bitmap || bitmap_size != m_cached_painted_size || !m_cached_texture.ptr()) {
            auto* bytes = g_bytes_new_static(bitmap->scanline_u8(0), bitmap->pitch() * painted_height);
            GObjectPtr builder { gdk_memory_texture_builder_new() };
            gdk_memory_texture_builder_set_bytes(GDK_MEMORY_TEXTURE_BUILDER(builder.ptr()), bytes);
            gdk_memory_texture_builder_set_stride(GDK_MEMORY_TEXTURE_BUILDER(builder.ptr()), bitmap->pitch());
            gdk_memory_texture_builder_set_width(GDK_MEMORY_TEXTURE_BUILDER(builder.ptr()), painted_width);
            gdk_memory_texture_builder_set_height(GDK_MEMORY_TEXTURE_BUILDER(builder.ptr()), painted_height);
            gdk_memory_texture_builder_set_format(GDK_MEMORY_TEXTURE_BUILDER(builder.ptr()), GDK_MEMORY_B8G8R8A8_PREMULTIPLIED);

            if (m_cached_texture.ptr()) {
                gdk_memory_texture_builder_set_update_texture(GDK_MEMORY_TEXTURE_BUILDER(builder.ptr()), m_cached_texture);
                cairo_rectangle_int_t full_rect = { 0, 0, painted_width, painted_height };
                auto* update_region = cairo_region_create_rectangle(&full_rect);
                gdk_memory_texture_builder_set_update_region(GDK_MEMORY_TEXTURE_BUILDER(builder.ptr()), update_region);
                cairo_region_destroy(update_region);
            }

            m_cached_texture = GObjectPtr<GdkTexture> { gdk_memory_texture_builder_build(GDK_MEMORY_TEXTURE_BUILDER(builder.ptr())) };
            g_bytes_unref(bytes);

            m_cached_bitmap = bitmap;
            m_cached_painted_size = bitmap_size;
        }

        auto device_pixel_ratio = m_device_pixel_ratio;
        auto draw_width = static_cast<float>(painted_width / device_pixel_ratio);
        auto draw_height = static_cast<float>(painted_height / device_pixel_ratio);

        graphene_rect_t texture_rect = GRAPHENE_RECT_INIT(0, 0, draw_width, draw_height);
        gtk_snapshot_append_texture(snapshot, m_cached_texture, &texture_rect);

        // Fill uncovered areas with theme-appropriate background
        auto is_dark = adw_style_manager_get_dark(adw_style_manager_get_default());
        GdkRGBA bg = is_dark ? GdkRGBA { 0.14, 0.14, 0.14, 1.0 } : GdkRGBA { 1.0, 1.0, 1.0, 1.0 };

        if (draw_width < width) {
            graphene_rect_t right_rect = GRAPHENE_RECT_INIT(draw_width, 0, static_cast<float>(width) - draw_width, static_cast<float>(height));
            gtk_snapshot_append_color(snapshot, &bg, &right_rect);
        }
        if (draw_height < height) {
            graphene_rect_t bottom_rect = GRAPHENE_RECT_INIT(0, draw_height, static_cast<float>(width), static_cast<float>(height) - draw_height);
            gtk_snapshot_append_color(snapshot, &bg, &bottom_rect);
        }
    } else {
        auto is_dark = adw_style_manager_get_dark(adw_style_manager_get_default());
        GdkRGBA bg = is_dark ? GdkRGBA { 0.14, 0.14, 0.14, 1.0 } : GdkRGBA { 1.0, 1.0, 1.0, 1.0 };
        graphene_rect_t full_rect = GRAPHENE_RECT_INIT(0, 0, static_cast<float>(width), static_cast<float>(height));
        gtk_snapshot_append_color(snapshot, &bg, &full_rect);
    }
}

void WebContentView::update_viewport_size()
{
    if (!m_widget)
        return;
    auto width = gtk_widget_get_width(GTK_WIDGET(m_widget));
    auto height = gtk_widget_get_height(GTK_WIDGET(m_widget));
    update_viewport_size(width, height);
}

void WebContentView::update_viewport_size(int width, int height)
{
    auto device_pixel_ratio = m_device_pixel_ratio;
    m_viewport_size = { static_cast<int>(width * device_pixel_ratio), static_cast<int>(height * device_pixel_ratio) };
    handle_resize();
}

void WebContentView::set_device_pixel_ratio(double device_pixel_ratio)
{
    m_device_pixel_ratio = device_pixel_ratio;
    update_viewport_size();
}

Web::DevicePixelSize WebContentView::viewport_size() const
{
    return { m_viewport_size.width(), m_viewport_size.height() };
}

Gfx::IntPoint WebContentView::to_content_position(Gfx::IntPoint widget_position) const
{
    return widget_position;
}

Gfx::IntPoint WebContentView::to_widget_position(Gfx::IntPoint content_position) const
{
    return content_position;
}

void WebContentView::initialize_client(CreateNewClient create_new_client)
{
    ViewImplementation::initialize_client(create_new_client);
    update_palette();
    update_screen_rects();
}

void WebContentView::update_zoom()
{
    ViewImplementation::update_zoom();
    gtk_widget_queue_draw(GTK_WIDGET(m_widget));

    if (on_zoom_level_changed)
        on_zoom_level_changed();
}

void WebContentView::set_has_focus(bool has_focus)
{
    client().async_set_has_focus(page_id(), has_focus);
}

void WebContentView::update_palette()
{
    auto is_dark = adw_style_manager_get_dark(adw_style_manager_get_default());
    auto theme_file = is_dark ? "Dark"sv : "Default"sv;
    auto theme_ini = MUST(Core::Resource::load_from_uri(MUST(String::formatted("resource://themes/{}.ini", theme_file))));
    auto theme_or_error = Gfx::load_system_theme(theme_ini->filesystem_path().to_byte_string());
    if (theme_or_error.is_error())
        return;
    auto theme = theme_or_error.release_value();
    set_preferred_color_scheme(is_dark ? Web::CSS::PreferredColorScheme::Dark : Web::CSS::PreferredColorScheme::Light);
    client().async_update_system_theme(page_id(), move(theme));
}

void WebContentView::update_screen_rects()
{
    auto* display = gdk_display_get_default();
    if (!display)
        return;

    auto* monitors = gdk_display_get_monitors(display);
    auto n_monitors = g_list_model_get_n_items(monitors);

    Vector<Web::DevicePixelRect> screen_rects;
    size_t main_screen_index = 0;

    for (guint i = 0; i < n_monitors; i++) {
        GObjectPtr monitor { g_list_model_get_item(monitors, i) };
        GdkRectangle geometry;
        gdk_monitor_get_geometry(GDK_MONITOR(monitor.ptr()), &geometry);
        auto scale = gdk_monitor_get_scale_factor(GDK_MONITOR(monitor.ptr()));

        screen_rects.append(Web::DevicePixelRect {
            geometry.x * scale,
            geometry.y * scale,
            geometry.width * scale,
            geometry.height * scale });
    }

    if (screen_rects.is_empty())
        screen_rects.append(Web::DevicePixelRect { 0, 0, 1920, 1080 });

    client().async_update_screen_rects(page_id(), move(screen_rects), main_screen_index);
}

}
