/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Bitmap.h>
#include <LibGfx/ShareableBitmap.h>
#include <LibWeb/Crypto/Crypto.h>
#include <LibWebView/HelperProcess.h>
#include <LibWebView/Utilities.h>
#include <UI/Headless/Application.h>
#include <UI/Headless/HeadlessWebView.h>

namespace Ladybird {

static Web::DevicePixelRect const screen_rect { 0, 0, 1920, 1080 };

HeadlessWebView::HeadlessWebView(Core::AnonymousBuffer theme, Web::DevicePixelSize viewport_size)
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
        auto web_worker_paths = MUST(WebView::get_paths_for_helper_process("WebWorker"sv));
        auto worker_client = MUST(WebView::launch_web_worker_process(web_worker_paths));

        return worker_client->clone_transport();
    };

    on_reposition_window = [this](auto position) {
        client().async_set_window_position(m_client_state.page_index, position.template to_type<Web::DevicePixels>());

        client().async_did_update_window_rect(m_client_state.page_index);
    };

    on_resize_window = [this](auto size) {
        m_viewport_size = size.template to_type<Web::DevicePixels>();

        client().async_set_window_size(m_client_state.page_index, m_viewport_size);
        client().async_set_viewport_size(m_client_state.page_index, m_viewport_size);

        client().async_did_update_window_rect(m_client_state.page_index);
    };

    on_restore_window = [this]() {
        set_system_visibility_state(Web::HTML::VisibilityState::Visible);
    };

    on_minimize_window = [this]() {
        set_system_visibility_state(Web::HTML::VisibilityState::Hidden);
    };

    on_maximize_window = [this]() {
        m_viewport_size = screen_rect.size();

        client().async_set_window_position(m_client_state.page_index, screen_rect.location());
        client().async_set_window_size(m_client_state.page_index, screen_rect.size());
        client().async_set_viewport_size(m_client_state.page_index, screen_rect.size());

        client().async_did_update_window_rect(m_client_state.page_index);
    };

    on_fullscreen_window = [this]() {
        m_viewport_size = screen_rect.size();

        client().async_set_window_position(m_client_state.page_index, screen_rect.location());
        client().async_set_window_size(m_client_state.page_index, screen_rect.size());
        client().async_set_viewport_size(m_client_state.page_index, screen_rect.size());

        client().async_did_update_window_rect(m_client_state.page_index);
    };

    on_request_alert = [this](auto const&) {
        m_pending_dialog = Web::Page::PendingDialog::Alert;
    };

    on_request_confirm = [this](auto const&) {
        m_pending_dialog = Web::Page::PendingDialog::Confirm;
    };

    on_request_prompt = [this](auto const&, auto const& prompt_text) {
        m_pending_dialog = Web::Page::PendingDialog::Prompt;
        m_pending_prompt_text = prompt_text;
    };

    on_request_set_prompt_text = [this](auto const& prompt_text) {
        m_pending_prompt_text = prompt_text;
    };

    on_request_accept_dialog = [this]() {
        switch (m_pending_dialog) {
        case Web::Page::PendingDialog::None:
            VERIFY_NOT_REACHED();
            break;
        case Web::Page::PendingDialog::Alert:
            alert_closed();
            break;
        case Web::Page::PendingDialog::Confirm:
            confirm_closed(true);
            break;
        case Web::Page::PendingDialog::Prompt:
            prompt_closed(move(m_pending_prompt_text));
            break;
        }

        m_pending_dialog = Web::Page::PendingDialog::None;
    };

    on_request_dismiss_dialog = [this]() {
        switch (m_pending_dialog) {
        case Web::Page::PendingDialog::None:
            VERIFY_NOT_REACHED();
            break;
        case Web::Page::PendingDialog::Alert:
            alert_closed();
            break;
        case Web::Page::PendingDialog::Confirm:
            confirm_closed(false);
            break;
        case Web::Page::PendingDialog::Prompt:
            prompt_closed({});
            break;
        }

        m_pending_dialog = Web::Page::PendingDialog::None;
        m_pending_prompt_text.clear();
    };

    m_system_visibility_state = Web::HTML::VisibilityState::Visible;
}

NonnullOwnPtr<HeadlessWebView> HeadlessWebView::create(Core::AnonymousBuffer theme, Web::DevicePixelSize window_size)
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
        auto request_server_socket = WebView::connect_new_request_server_client().release_value_but_fixme_should_propagate_errors();
        auto image_decoder_socket = WebView::connect_new_image_decoder_client().release_value_but_fixme_should_propagate_errors();

        auto web_content_paths = WebView::get_paths_for_helper_process("WebContent"sv).release_value_but_fixme_should_propagate_errors();
        m_client_state.client = WebView::launch_web_content_process(*this, web_content_paths, move(image_decoder_socket), move(request_server_socket)).release_value_but_fixme_should_propagate_errors();
    } else {
        m_client_state.client->register_view(m_client_state.page_index, *this);
    }

    m_client_state.client_handle = MUST(Web::Crypto::generate_random_uuid());
    client().async_set_window_handle(m_client_state.page_index, m_client_state.client_handle);

    client().async_update_system_theme(m_client_state.page_index, m_theme);
    client().async_set_viewport_size(m_client_state.page_index, viewport_size());
    client().async_set_window_size(m_client_state.page_index, viewport_size());
    client().async_update_screen_rects(m_client_state.page_index, { screen_rect }, 0);

    set_system_visibility_state(m_system_visibility_state);

    if (Application::chrome_options().allow_popups == WebView::AllowPopups::Yes)
        client().async_debug_request(m_client_state.page_index, "block-pop-ups"sv, "off"sv);

    if (auto const& web_driver_ipc_path = Application::chrome_options().webdriver_content_ipc_path; web_driver_ipc_path.has_value())
        client().async_connect_to_webdriver(m_client_state.page_index, *web_driver_ipc_path);

    m_client_state.client->on_web_content_process_crash = [this] {
        Core::deferred_invoke([this] {
            handle_web_content_process_crash(LoadErrorPage::No);

            if (on_web_content_crashed)
                on_web_content_crashed();
        });
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
