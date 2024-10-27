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
#include <LibWeb/Crypto/Crypto.h>

namespace Ladybird {

HeadlessWebView::HeadlessWebView(Core::AnonymousBuffer theme, Gfx::IntSize viewport_size)
    : m_theme(move(theme))
    , m_viewport_size(viewport_size)
    , m_test_promise(TestPromise::construct())
{
    on_new_web_view = [this](auto, auto, Optional<u64> page_index) {
        if (page_index.has_value()) {
            auto& web_view = Application::the().create_child_web_view(*this, *page_index);
            return web_view.handle();
        }

        auto& web_view = Application::the().create_web_view(m_theme, m_viewport_size);
        return web_view.handle();
    };

    on_request_worker_agent = []() {
        auto web_worker_paths = MUST(get_paths_for_helper_process("WebWorker"sv));
        auto worker_client = MUST(launch_web_worker_process(web_worker_paths, Application::request_client()));

        return worker_client->clone_transport();
    };
}

NonnullOwnPtr<HeadlessWebView> HeadlessWebView::create(Core::AnonymousBuffer theme, Gfx::IntSize window_size)
{
    auto view = adopt_own(*new HeadlessWebView(move(theme), window_size));
    view->initialize_client(CreateNewClient::Yes);

    return view;
}

NonnullOwnPtr<HeadlessWebView> HeadlessWebView::create_child(HeadlessWebView const& parent, u64 page_index)
{
    auto view = adopt_own(*new HeadlessWebView(parent.m_theme, parent.m_viewport_size));

    view->m_client_state.client = parent.client();
    view->m_client_state.page_index = page_index;
    view->initialize_client(CreateNewClient::No);

    return view;
}

void HeadlessWebView::initialize_client(CreateNewClient create_new_client)
{
    if (create_new_client == CreateNewClient::Yes) {
        auto request_server_socket = connect_new_request_server_client(Application::request_client()).release_value_but_fixme_should_propagate_errors();
        auto image_decoder_socket = connect_new_image_decoder_client(Application::image_decoder_client()).release_value_but_fixme_should_propagate_errors();

        auto web_content_paths = get_paths_for_helper_process("WebContent"sv).release_value_but_fixme_should_propagate_errors();
        m_client_state.client = launch_web_content_process(*this, web_content_paths, move(image_decoder_socket), move(request_server_socket)).release_value_but_fixme_should_propagate_errors();
    } else {
        m_client_state.client->register_view(m_client_state.page_index, *this);
    }

    m_client_state.client_handle = MUST(Web::Crypto::generate_random_uuid());
    client().async_set_window_handle(m_client_state.page_index, m_client_state.client_handle);

    client().async_update_system_theme(m_client_state.page_index, m_theme);
    client().async_set_viewport_size(m_client_state.page_index, viewport_size());
    client().async_set_window_size(m_client_state.page_index, viewport_size());

    Vector<Web::DevicePixelRect> screen_rects { Web::DevicePixelRect { 0, 0, 1920, 1080 } };
    client().async_update_screen_rects(m_client_state.page_index, move(screen_rects), 0);

    if (Application::chrome_options().allow_popups == WebView::AllowPopups::Yes)
        client().async_debug_request(m_client_state.page_index, "block-pop-ups"sv, "off"sv);

    if (auto const& web_driver_ipc_path = Application::chrome_options().webdriver_content_ipc_path; web_driver_ipc_path.has_value())
        client().async_connect_to_webdriver(m_client_state.page_index, *web_driver_ipc_path);

    m_client_state.client->on_web_content_process_crash = [this] {
        warnln("\033[31;1mWebContent Crashed!!\033[0m");
        warnln("    Last page loaded: {}", url());
        VERIFY_NOT_REACHED();
    };
}

void HeadlessWebView::clear_content_filters()
{
    client().async_set_content_filters(m_client_state.page_index, {});
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
