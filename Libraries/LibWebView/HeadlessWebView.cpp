/*
 * Copyright (c) 2024-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/HeadlessWebView.h>

namespace WebView {

static Web::DevicePixelRect const screen_rect { 0, 0, 1920, 1080 };

NonnullOwnPtr<HeadlessWebView> HeadlessWebView::create(Core::AnonymousBuffer theme, Web::DevicePixelSize window_size)
{
    auto view = adopt_own(*new HeadlessWebView(move(theme), window_size));
    view->initialize_client(CreateNewClient::Yes);

    return view;
}

NonnullOwnPtr<HeadlessWebView> HeadlessWebView::create_child(HeadlessWebView& parent, u64 page_index)
{
    auto view = adopt_own(*new HeadlessWebView(parent.m_theme, parent.m_viewport_size));

    view->m_client_state.client = parent.client();
    view->m_client_state.page_index = page_index;
    view->initialize_client(CreateNewClient::No);

    return view;
}

HeadlessWebView::HeadlessWebView(Core::AnonymousBuffer theme, Web::DevicePixelSize viewport_size)
    : m_theme(move(theme))
    , m_viewport_size(viewport_size)
{
    on_new_web_view = [this](auto, auto, Optional<u64> page_index) {
        auto web_view = page_index.has_value()
            ? HeadlessWebView::create_child(*this, *page_index)
            : HeadlessWebView::create(m_theme, m_viewport_size);

        // Propagate crashes from child views to parent, so parent tests don't hang
        // waiting for a child that crashed.
        web_view->on_web_content_crashed = [this]() {
            if (on_web_content_crashed)
                on_web_content_crashed();
        };

        m_child_web_views.append(move(web_view));
        return m_child_web_views.last()->handle();
    };

    on_reposition_window = [this](auto position) {
        m_previous_dimensions.set_location(position.template to_type<Web::DevicePixels>());
        client().async_set_window_position(m_client_state.page_index, position.template to_type<Web::DevicePixels>());

        client().async_did_update_window_rect(m_client_state.page_index);
    };

    on_resize_window = [this](auto size) {
        m_viewport_size = size.template to_type<Web::DevicePixels>();

        client().async_set_window_size(m_client_state.page_index, m_viewport_size);
        client().async_set_viewport(m_client_state.page_index, m_viewport_size, m_device_pixel_ratio);

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
        m_previous_dimensions = screen_rect;

        client().async_set_window_position(m_client_state.page_index, screen_rect.location());
        client().async_set_window_size(m_client_state.page_index, screen_rect.size());
        client().async_set_viewport(m_client_state.page_index, screen_rect.size(), m_device_pixel_ratio);

        client().async_did_update_window_rect(m_client_state.page_index);
    };

    on_fullscreen_window = [this]() {
        m_previous_dimensions.set_size(m_viewport_size);
        m_viewport_size = screen_rect.size();

        client().async_set_window_position(m_client_state.page_index, screen_rect.location());
        client().async_set_window_size(m_client_state.page_index, screen_rect.size());
        client().async_set_viewport(m_client_state.page_index, screen_rect.size(), m_device_pixel_ratio);

        client().async_did_update_window_rect(m_client_state.page_index);
    };

    on_exit_fullscreen_window = [this]() {
        m_viewport_size = m_previous_dimensions.size();
        client().async_set_window_position(m_client_state.page_index, m_previous_dimensions.location());
        client().async_set_window_size(m_client_state.page_index, m_previous_dimensions.size());
        client().async_set_viewport(m_client_state.page_index, m_previous_dimensions.size(), m_device_pixel_ratio);

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

void HeadlessWebView::initialize_client(CreateNewClient create_new_client)
{
    ViewImplementation::initialize_client(create_new_client);

    client().async_update_system_theme(m_client_state.page_index, m_theme);
    client().async_set_viewport(m_client_state.page_index, viewport_size(), m_device_pixel_ratio);
    client().async_set_window_size(m_client_state.page_index, viewport_size());
    client().async_update_screen_rects(m_client_state.page_index, { { screen_rect } }, 0);
}

void HeadlessWebView::reset_viewport_size(Web::DevicePixelSize size)
{
    m_viewport_size = size;

    client().async_set_window_size(m_client_state.page_index, m_viewport_size);
    client().async_set_viewport(m_client_state.page_index, m_viewport_size, m_device_pixel_ratio);

    client().async_did_update_window_rect(m_client_state.page_index);
}

void HeadlessWebView::update_zoom()
{
    ViewImplementation::update_zoom();
}

}
