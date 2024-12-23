/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Bitmap.h>
#include <LibGfx/ShareableBitmap.h>
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
    ViewImplementation::initialize_client(create_new_client);

    client().async_update_system_theme(m_client_state.page_index, m_theme);
    client().async_set_viewport_size(m_client_state.page_index, viewport_size());
    client().async_set_window_size(m_client_state.page_index, viewport_size());
    client().async_update_screen_rects(m_client_state.page_index, { screen_rect }, 0);
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
    m_pending_screenshot.clear();
    m_pending_dialog = Web::Page::PendingDialog::None;
    m_pending_prompt_text.clear();

    m_test_promise->resolve(move(completion));
}

void HeadlessWebView::update_zoom()
{
    client().async_set_device_pixels_per_css_pixel(m_client_state.page_index, m_device_pixel_ratio * m_zoom_level);
    client().async_set_viewport_size(m_client_state.page_index, m_viewport_size);
}

}
