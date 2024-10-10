/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Ladybird/Headless/Application.h>
#include <Ladybird/Headless/HeadlessWebView.h>
#include <Ladybird/HelperProcess.h>
#include <Ladybird/Utilities.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ShareableBitmap.h>

namespace Ladybird {

HeadlessWebView::HeadlessWebView(Gfx::IntSize viewport_size)
    : m_viewport_size(viewport_size)
    , m_test_promise(TestPromise::construct())
{
    on_request_worker_agent = []() {
        auto web_worker_paths = MUST(get_paths_for_helper_process("WebWorker"sv));
        auto worker_client = MUST(launch_web_worker_process(web_worker_paths, Application::request_client()));

        return worker_client->dup_socket();
    };
}

ErrorOr<NonnullOwnPtr<HeadlessWebView>> HeadlessWebView::create(Core::AnonymousBuffer theme, Gfx::IntSize window_size)
{
    auto view = TRY(adopt_nonnull_own_or_enomem(new (nothrow) HeadlessWebView(window_size)));

    auto request_server_socket = TRY(connect_new_request_server_client(Application::request_client()));
    auto image_decoder_socket = TRY(connect_new_image_decoder_client(Application::image_decoder_client()));

    auto candidate_web_content_paths = TRY(get_paths_for_helper_process("WebContent"sv));
    view->m_client_state.client = TRY(launch_web_content_process(*view, candidate_web_content_paths, move(image_decoder_socket), move(request_server_socket)));

    view->client().async_update_system_theme(0, move(theme));
    view->client().async_set_viewport_size(0, view->viewport_size());
    view->client().async_set_window_size(0, view->viewport_size());

    if (WebView::Application::chrome_options().allow_popups == WebView::AllowPopups::Yes)
        view->client().async_debug_request(0, "block-pop-ups"sv, "off"sv);

    if (auto web_driver_ipc_path = WebView::Application::chrome_options().webdriver_content_ipc_path; web_driver_ipc_path.has_value())
        view->client().async_connect_to_webdriver(0, *web_driver_ipc_path);

    view->m_client_state.client->on_web_content_process_crash = [&view = *view] {
        warnln("\033[31;1mWebContent Crashed!!\033[0m");
        warnln("    Last page loaded: {}", view.url());
        VERIFY_NOT_REACHED();
    };

    return view;
}

void HeadlessWebView::clear_content_filters()
{
    client().async_set_content_filters(0, {});
}

NonnullRefPtr<Core::Promise<RefPtr<Gfx::Bitmap>>> HeadlessWebView::take_screenshot()
{
    VERIFY(!m_pending_screenshot);

    m_pending_screenshot = Core::Promise<RefPtr<Gfx::Bitmap>>::construct();
    client().async_take_document_screenshot(0);

    return *m_pending_screenshot;
}

void HeadlessWebView::did_receive_screenshot(Badge<WebView::WebContentClient>, Gfx::ShareableBitmap const& screenshot)
{
    VERIFY(m_pending_screenshot);

    auto pending_screenshot = move(m_pending_screenshot);
    pending_screenshot->resolve(screenshot.bitmap());
}

void HeadlessWebView::on_test_complete(TestCompletion completion)
{
    m_test_promise->resolve(move(completion));
}

}
