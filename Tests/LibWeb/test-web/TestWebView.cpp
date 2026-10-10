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
    view->initialize_tab(Web::HTML::VisibilityState::Visible);

    return view;
}

TestWebView::TestWebView(Core::AnonymousBuffer theme, Web::DevicePixelSize viewport_size)
    : WebView::HeadlessWebView(move(theme), viewport_size)
    , m_test_promise(TestPromise::construct())
{
}

void TestWebView::clear_content_blockers()
{
    page().client().async_set_content_blockers(MUST(Core::AnonymousBuffer::create_with_size(0)));
}

// Page::perform_per_test_cleanup() resets the state that only tests move and that would otherwise outlive the test
// that set it (force-dark and the line-box borders ride on the navigable, the preferred-color-scheme override on the
// page). It's the harness that has to ask for it: test-web takes a test's screenshot after the test signals that it's
// done, so the page can't reset itself at that point — and a test that times out or crashes never signals at all.
void TestWebView::perform_per_test_cleanup()
{
    debug_request("perform-per-test-cleanup"sv);
}

// The emulated position lives on the page, so a test that moves it would otherwise hand its position to the next test.
void TestWebView::reset_geolocation_emulated_position()
{
    geolocation_settings_changed();
}

NonnullRefPtr<Core::Promise<Empty>> TestWebView::reset_session_history()
{
    return WebView::ViewImplementation::reset_session_history_for_testing();
}

pid_t TestWebView::web_content_pid() const
{
    return page().client().pid();
}

NonnullRefPtr<Core::Promise<RefPtr<Gfx::Bitmap const>>> TestWebView::take_screenshot()
{
    VERIFY(!m_pending_screenshot);

    m_pending_screenshot = Core::Promise<RefPtr<Gfx::Bitmap const>>::construct();
    page().async_take_document_screenshot();

    return *m_pending_screenshot;
}

void TestWebView::did_receive_screenshot(Badge<WebView::WebContentPage>, Gfx::ShareableBitmap const& screenshot)
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
    m_pending_dialog = Web::PendingDialog::None;
    m_pending_prompt_text.clear();
    m_is_fullscreen = Web::ViewportIsFullscreen::No;

    page().async_set_viewport(viewport_size(), 1.0, Web::ViewportIsFullscreen::No);

    m_test_promise->resolve(move(completion));
}

}
