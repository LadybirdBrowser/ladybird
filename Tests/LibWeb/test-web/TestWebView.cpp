/*
 * Copyright (c) 2024-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "TestWebView.h"

#include <LibGfx/Bitmap.h>
#include <LibGfx/ShareableBitmap.h>

namespace TestWeb {

NonnullOwnPtr<TestWebView> TestWebView::create(Core::AnonymousBuffer theme, Web::DevicePixelSize window_size)
{
    auto view = adopt_own(*new TestWebView(move(theme), window_size));
    view->initialize_client(CreateNewClient::Yes);

    return view;
}

TestWebView::TestWebView(Core::AnonymousBuffer theme, Web::DevicePixelSize viewport_size)
    : WebView::HeadlessWebView(move(theme), viewport_size)
    , m_test_promise(TestPromise::construct())
{
}

void TestWebView::clear_content_filters()
{
    client().async_set_content_filters(m_client_state.page_index, {});
}

pid_t TestWebView::web_content_pid() const
{
    return client().pid();
}

NonnullRefPtr<Core::Promise<RefPtr<Gfx::Bitmap const>>> TestWebView::take_screenshot(WebView::ViewImplementation::ScreenshotType type)
{
    return take_screenshot_bitmap(type);
}

void TestWebView::on_test_complete(TestCompletion completion)
{
    clear_pending_screenshot_requests();
    m_pending_dialog = Web::Page::PendingDialog::None;
    m_pending_prompt_text.clear();
    m_is_fullscreen = Web::ViewportIsFullscreen::No;
    client().async_set_viewport(m_client_state.page_index, viewport_size(), 1.0, Web::ViewportIsFullscreen::No);

    m_test_promise->resolve(move(completion));
}

}
