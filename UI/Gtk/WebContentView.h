/*
 * Copyright (c) 2026, Johan Dahlin <jdahlin@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/OwnPtr.h>
#include <LibGfx/Forward.h>
#include <LibWeb/Forward.h>
#include <LibWebView/ViewImplementation.h>

#include <UI/Gtk/GLibPtr.h>
#include <UI/Gtk/Widgets/LadybirdWebView.h>

namespace Ladybird {

class WebContentView final : public WebView::ViewImplementation {
public:
    WebContentView(LadybirdWebView* widget, RefPtr<WebView::WebContentClient> parent_client = nullptr, size_t page_index = 0);
    virtual ~WebContentView() override;

    LadybirdWebView* gtk_widget() const { return m_widget; }

    void update_viewport_size();
    void update_viewport_size(int width, int height);
    void set_device_pixel_ratio(double device_pixel_ratio);
    void set_widget(LadybirdWebView* widget) { m_widget = widget; }
    void paint(GtkSnapshot* snapshot);

    void update_palette();
    void update_screen_rects();

    WebView::WebContentClient& client() { return ViewImplementation::client(); }
    void set_has_focus(bool has_focus);

    Gfx::IntPoint to_content_position(Gfx::IntPoint widget_position) const override;
    Gfx::IntPoint to_widget_position(Gfx::IntPoint content_position) const override;

    void enqueue_native_event(Web::MouseEvent::Type, double x, double y, unsigned button, GdkModifierType state, int click_count);
    void enqueue_native_event(Web::KeyEvent::Type, guint keyval, GdkModifierType state);

    Function<void()> on_zoom_level_changed;

private:
    Web::DevicePixelSize viewport_size() const override;
    void initialize_client(CreateNewClient = CreateNewClient::Yes) override;
    void update_zoom() override;

    void finish_handling_key_event(Web::KeyEvent const&);

    LadybirdWebView* m_widget { nullptr };
    Gfx::IntSize m_viewport_size;

    // Cached texture to avoid recreation on every snapshot
    GObjectPtr<GdkTexture> m_cached_texture;
    Gfx::Bitmap const* m_cached_bitmap { nullptr };
    Gfx::IntSize m_cached_painted_size;
};

}
