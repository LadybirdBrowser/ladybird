/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebView/ViewImplementation.h>
#include <android/bitmap.h>
#include <jni.h>

namespace Ladybird {

class WebViewImplementationNative : public WebView::ViewImplementation {
public:
    WebViewImplementationNative(jobject thiz);

    virtual Web::DevicePixelSize viewport_size() const override { return m_viewport_size; }
    virtual Gfx::IntPoint to_content_position(Gfx::IntPoint p) const override { return p; }
    virtual Gfx::IntPoint to_widget_position(Gfx::IntPoint p) const override { return p; }
    virtual void update_zoom() override
    {
        client().async_set_zoom_level(page_id(), m_zoom_level);
    }

    NonnullRefPtr<WebView::WebContentClient> bind_web_content_client();

    virtual void initialize_client(CreateNewClient) override;

    void paint_into_bitmap(void* android_bitmap_raw, AndroidBitmapInfo const& info);

    void set_viewport_geometry(int w, int h);
    void set_zoom_level(double zoom_level);
    void set_device_pixel_ratio(double f);

    void mouse_event(Web::MouseEvent::Type event_type, float x, float y, float raw_x, float raw_y);
    void wheel_event(float x, float y, float raw_x, float raw_y, int wheel_delta_x, int wheel_delta_y);

    static jclass global_class_reference;
    static jmethodID bind_webcontent_method;
    static jmethodID invalidate_layout_method;
    static jmethodID on_load_start_method;
    static jmethodID on_load_finish_method;
    static jmethodID on_title_change_method;
    static jmethodID on_url_change_method;
    static jmethodID on_find_in_page_method;
    static jmethodID on_link_hover_method;

    jobject java_instance() const { return m_java_instance; }

private:
    jobject m_java_instance = nullptr;
    Web::DevicePixelSize m_viewport_size;
};

}
