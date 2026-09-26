/*
 * Copyright (c) 2024-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "TestWeb.h"

#include <AK/Badge.h>
#include <AK/RefPtr.h>
#include <LibCompositing/PixelUnits.h>
#include <LibCore/Forward.h>
#include <LibCore/Promise.h>
#include <LibGfx/Forward.h>
#include <LibWebView/HeadlessWebView.h>

namespace TestWeb {

class TestWebView final : public WebView::HeadlessWebView {
public:
    static NonnullOwnPtr<TestWebView> create(Core::AnonymousBuffer theme, Compositing::DevicePixelSize window_size);

    void clear_content_blockers();
    void perform_per_test_cleanup();
    void reset_geolocation_emulated_position();
    NonnullRefPtr<Core::Promise<Empty>> reset_session_history();
    pid_t web_content_pid() const;

    NonnullRefPtr<Core::Promise<RefPtr<Gfx::Bitmap const>>> take_screenshot();

    TestPromise& test_promise() { return *m_test_promise; }
    void reset_test_promise() { m_test_promise = TestPromise::construct(); }
    void on_test_complete(TestCompletion);

private:
    TestWebView(Core::AnonymousBuffer theme, Compositing::DevicePixelSize viewport_size);

    virtual Web::Clipboard::SystemClipboardItem clipboard_item() const override { return m_clipboard_item; }
    virtual void insert_clipboard_item(Web::Clipboard::SystemClipboardItem item) override { m_clipboard_item = move(item); }

    virtual void did_receive_screenshot(Badge<WebView::WebContentPage>, Gfx::ShareableBitmap const& screenshot) override;
    RefPtr<Core::Promise<RefPtr<Gfx::Bitmap const>>> m_pending_screenshot;

    Web::Clipboard::SystemClipboardItem m_clipboard_item;

    NonnullRefPtr<TestPromise> m_test_promise;
};

}
