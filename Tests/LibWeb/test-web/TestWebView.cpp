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

NonnullRefPtr<Core::Promise<RefPtr<Gfx::Bitmap const>>> TestWebView::take_screenshot()
{
    VERIFY(!m_pending_screenshot);

    m_pending_screenshot = Core::Promise<RefPtr<Gfx::Bitmap const>>::construct();
    client().async_take_document_screenshot(0);

    return *m_pending_screenshot;
}

void TestWebView::did_receive_screenshot(Badge<WebView::WebContentClient>, Gfx::ShareableBitmap const& screenshot)
{
    if (!m_pending_screenshot) {
        static bool warned_about_stray_screenshot = false;
        if (!warned_about_stray_screenshot) {
            warned_about_stray_screenshot = true;
            warnln("Ignoring screenshot response with no pending request");
        }
        return;
    }

    auto pending_screenshot = move(m_pending_screenshot);
    pending_screenshot->resolve(screenshot.bitmap());
}

void TestWebView::on_test_complete(TestCompletion completion)
{
    m_pending_screenshot.clear();
    m_pending_dialog = Web::Page::PendingDialog::None;
    m_pending_prompt_text.clear();
    client().async_set_device_pixel_ratio(m_client_state.page_index, 1.0);

    m_test_promise->resolve(completion);
}

}
