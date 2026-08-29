/*
 * Copyright (c) 2024-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "TestWebView.h"

#include "Application.h"

#include <LibCore/AnonymousBuffer.h>
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

void TestWebView::clear_content_blockers()
{
    client().async_set_content_blockers(m_client_state.page_index, MUST(Core::AnonymousBuffer::create_with_size(0)));
}

// Force-dark rides on the navigable, so a test that turns it on leaves it on for whatever runs next in this view.
void TestWebView::reset_force_dark()
{
    debug_request("set-force-dark"sv, "off"sv);
}

// Same story as force-dark above: the flag lives on the navigable and would otherwise outlive the test that set it.
void TestWebView::reset_line_box_borders()
{
    debug_request("set-line-box-borders"sv, "off"sv);
}

// Tests reach force-dark through internals, never through the browser's own setting: initialize_client() re-applies
// that setting to every new WebContent (process swaps and crash respawns included), at moments no test controls.
void TestWebView::force_dark_settings_changed()
{
}

NonnullRefPtr<Core::Promise<Empty>> TestWebView::reset_session_history()
{
    return WebView::ViewImplementation::reset_session_history_for_testing();
}

pid_t TestWebView::web_content_pid() const
{
    return client().pid();
}

NonnullRefPtr<Core::Promise<RefPtr<Gfx::Bitmap const>>> TestWebView::take_screenshot()
{
    VERIFY(!m_pending_screenshot);

    m_pending_screenshot = Core::Promise<RefPtr<Gfx::Bitmap const>>::construct();
    client().async_take_document_screenshot(page_id());

    return *m_pending_screenshot;
}

void TestWebView::did_receive_screenshot(Badge<WebView::WebContentClient>, Gfx::ShareableBitmap const& screenshot)
{
    // NOTE: The screenshot may arrive after a timeout already completed the test and cleared m_pending_screenshot.
    if (!m_pending_screenshot)
        return;

    auto pending_screenshot = move(m_pending_screenshot);
    pending_screenshot->resolve(screenshot.bitmap());
}

void TestWebView::on_test_complete(TestCompletion completion)
{
    m_pending_screenshot.clear();
    m_pending_dialog = Web::Page::PendingDialog::None;
    m_pending_prompt_text.clear();
    m_is_fullscreen = Web::ViewportIsFullscreen::No;
    client().async_set_viewport(m_client_state.page_index, viewport_size(), 1.0, Web::ViewportIsFullscreen::No);

    m_test_promise->resolve(move(completion));
}

}
