/*
 * Copyright (c) 2024-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibWebView/HeadlessWebView.h>

namespace WebView {

static Compositing::DevicePixelRect const screen_rect { 0, 0, 1920, 1080 };
static constexpr auto child_close_timeout_ms = 1000;

NonnullOwnPtr<HeadlessWebView> HeadlessWebView::create(Core::AnonymousBuffer theme, Compositing::DevicePixelSize window_size, IsPrivate is_private)
{
    auto view = adopt_own(*new HeadlessWebView(move(theme), window_size, is_private));
    view->initialize_client(CreateNewClient::Yes);

    return view;
}

NonnullOwnPtr<HeadlessWebView> HeadlessWebView::create_child(HeadlessWebView& parent, WebContentClient& page_process, Compositing::PageId page_index)
{
    // The child shares the WebContent client hosting its page, and with it that client's browsing session.
    auto view = adopt_own(*new HeadlessWebView(parent.m_theme, parent.m_viewport_size, parent.is_private()));

    page_process.register_view(page_index, *view);
    view->initialize_client(CreateNewClient::No);

    return view;
}

HeadlessWebView::HeadlessWebView(Core::AnonymousBuffer theme, Compositing::DevicePixelSize viewport_size, IsPrivate is_private)
    : ViewImplementation(is_private)
    , m_theme(move(theme))
    , m_viewport_size(viewport_size)
{
    on_new_web_view = [this](auto, auto, WebContentClient& page_process, Optional<Compositing::PageId> page_index) {
        auto web_view = page_index.has_value()
            ? HeadlessWebView::create_child(*this, page_process, *page_index)
            : HeadlessWebView::create(m_theme, m_viewport_size, this->is_private());

        auto* child_web_view = web_view.ptr();
        auto weak_this = make_weak_ptr<HeadlessWebView>();
        web_view->m_parent_web_view = weak_this;
        auto discard_child_web_view = [weak_this, child_web_view]() {
            if (weak_this)
                weak_this->discard_child_web_view(*child_web_view);
        };

        // Propagate crashes from child views to parent, so parent tests don't hang
        // waiting for a child that crashed.
        web_view->on_web_content_crashed = [child_web_view, discard_child_web_view](auto crash_reason) {
            child_web_view->propagate_web_content_crash(crash_reason);
            discard_child_web_view();
        };
        web_view->on_close = move(discard_child_web_view);

        m_child_web_views.append(move(web_view));
        return m_child_web_views.last()->handle();
    };

    on_reposition_window = [this](auto position) {
        m_previous_dimensions.set_location(position.template to_type<Compositing::DevicePixels>());
        client().async_set_window_position(page_id(), position.template to_type<Compositing::DevicePixels>());
    };

    on_resize_window = [this](auto size) {
        m_viewport_size = size.template to_type<Compositing::DevicePixels>();

        client().async_set_window_size(page_id(), m_viewport_size);
        handle_resize();
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

        client().async_set_window_position(page_id(), screen_rect.location());
        client().async_set_window_size(page_id(), screen_rect.size());
        handle_resize();
    };

    on_fullscreen_window = [this]() {
        m_previous_dimensions.set_size(m_viewport_size);
        m_viewport_size = screen_rect.size();

        client().async_set_window_position(page_id(), screen_rect.location());
        client().async_set_window_size(page_id(), screen_rect.size());
        set_is_fullscreen(Web::ViewportIsFullscreen::Yes);
    };

    on_exit_fullscreen_window = [this]() {
        m_viewport_size = m_previous_dimensions.size();

        client().async_set_window_position(page_id(), m_previous_dimensions.location());
        client().async_set_window_size(page_id(), m_previous_dimensions.size());
        set_is_fullscreen(Web::ViewportIsFullscreen::No);
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

    m_top_level_traversable.set_system_visibility_state(Web::HTML::VisibilityState::Visible);
}

void HeadlessWebView::propagate_web_content_crash(WebContentCrashReason crash_reason)
{
    if (!m_propagate_crashes_to_parent)
        return;

    if (m_parent_web_view) {
        m_parent_web_view->propagate_web_content_crash(crash_reason);
        return;
    }

    if (on_web_content_crashed)
        on_web_content_crashed(crash_reason);
}

void HeadlessWebView::discard_child_web_view(HeadlessWebView& child_web_view)
{
    auto* child_web_view_pointer = &child_web_view;
    Core::deferred_invoke([weak_this = make_weak_ptr<HeadlessWebView>(), child_web_view_pointer] {
        if (!weak_this)
            return;
        weak_this->m_child_web_views.remove_first_matching([child_web_view_pointer](auto const& child) {
            return child.ptr() == child_web_view_pointer;
        });
    });
}

void HeadlessWebView::schedule_forced_close()
{
    if (!m_forced_close_timer) {
        m_forced_close_timer = Core::Timer::create_single_shot(child_close_timeout_ms, [weak_this = make_weak_ptr<HeadlessWebView>()] {
            if (!weak_this || weak_this->handle().is_empty() || !weak_this->client().is_open())
                return;
            weak_this->force_close();
        });
    }

    if (!m_forced_close_timer->is_active())
        m_forced_close_timer->start();
}

void HeadlessWebView::initialize_client(CreateNewClient create_new_client, Optional<Web::HTML::CrossProcessId> initial_document_state_id)
{
    ViewImplementation::initialize_client(create_new_client, initial_document_state_id);

    client().async_update_system_theme(page_id(), m_theme);
    handle_resize();
    client().async_set_window_size(page_id(), viewport_size());
    client().async_update_screen_rects(page_id(), { { screen_rect } }, 0);
}

void HeadlessWebView::reset_viewport_size(Compositing::DevicePixelSize size)
{
    m_viewport_size = size;

    client().async_set_window_size(page_id(), m_viewport_size);
    handle_resize();
}

void HeadlessWebView::update_zoom()
{
    ViewImplementation::update_zoom();
}

}
