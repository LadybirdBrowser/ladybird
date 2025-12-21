/*
 * Copyright (c) 2024-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "TestWeb.h"

#include <AK/Badge.h>
#include <AK/RefPtr.h>
#include <LibCore/Forward.h>
#include <LibCore/Promise.h>
#include <LibGfx/Forward.h>
#include <LibWeb/PixelUnits.h>
#include <LibWebView/HeadlessWebView.h>

namespace TestWeb {

class TestWebView final : public WebView::HeadlessWebView {
public:
    static NonnullOwnPtr<TestWebView> create(Core::AnonymousBuffer theme, Web::DevicePixelSize window_size);

    void clear_content_filters();

    NonnullRefPtr<Core::Promise<RefPtr<Gfx::Bitmap const>>> take_screenshot();

    TestPromise& test_promise() { return *m_test_promise; }
    void on_test_complete(TestCompletion);

    void restart_web_content_process();
    size_t run_count() const { return m_run_count; }
    void increment_run_count() { ++m_run_count; }

private:
    TestWebView(Core::AnonymousBuffer theme, Web::DevicePixelSize viewport_size);

    virtual void did_receive_screenshot(Badge<WebView::WebContentClient>, Gfx::ShareableBitmap const& screenshot) override;

    RefPtr<Core::Promise<RefPtr<Gfx::Bitmap const>>> m_pending_screenshot;

    NonnullRefPtr<TestPromise> m_test_promise;

    size_t m_run_count { 0 };
};

}
