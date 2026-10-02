/*
 * Copyright (c) 2024-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/PictureInPictureWindow.h>

namespace WebView {

static Web::DevicePixelRect const screen_rect { 0, 0, 1920, 1080 };
static constexpr auto child_close_timeout_ms = 1000;

// A headless window is never shown, so it keeps the size it opens with until it is closed.
class HeadlessPictureInPictureWindow final : public PictureInPictureWindow {
public:
    AK_ALLOC_WITH_KMALLOC;

    HeadlessPictureInPictureWindow(Gfx::IntSize size, NonnullOwnPtr<HeadlessWebView> view)
        : m_size(size)
        , m_view(move(view))
    {
        report_page_close_of(*m_view);
    }

    virtual Gfx::IntSize size() const override { return m_size; }
    virtual String handle() const override { return m_view->handle(); }
    virtual void hide() override { }

private:
    Gfx::IntSize m_size;
    NonnullOwnPtr<HeadlessWebView> m_view;
};

NonnullOwnPtr<PictureInPictureWindow> HeadlessWebView::create_picture_in_picture_window(HeadlessWebView& requesting_view, CanonicalTraversable& traversable, Gfx::IntSize video_size)
{
    auto size = PictureInPictureWindow::initial_size(video_size, screen_rect.size().to_type<int>());
    auto view = create_child(requesting_view, traversable);
    view->reset_viewport_size(size.to_type<Web::DevicePixels>());
    return make<HeadlessPictureInPictureWindow>(size, move(view));
}

NonnullOwnPtr<HeadlessWebView> HeadlessWebView::create(Core::AnonymousBuffer theme, Web::DevicePixelSize window_size, IsPrivate is_private)
{
    auto view = adopt_own(*new HeadlessWebView(move(theme), window_size, is_private));
    view->initialize_tab(Web::HTML::VisibilityState::Visible);

    return view;
}

NonnullOwnPtr<HeadlessWebView> HeadlessWebView::create_child(HeadlessWebView& parent, CanonicalTraversable& traversable)
{
    auto view = adopt_own(*new HeadlessWebView(parent.m_theme, parent.m_viewport_size, parent.is_private()));
    view->initialize_tab(Web::HTML::VisibilityState::Visible, traversable);

    return view;
}

HeadlessWebView::HeadlessWebView(Core::AnonymousBuffer theme, Web::DevicePixelSize viewport_size, IsPrivate is_private)
    : ViewImplementation(is_private)
    , m_theme(move(theme))
    , m_viewport_size(viewport_size)
{
    on_new_web_view = [this](auto, auto, CanonicalTraversable& traversable) {
        auto web_view = HeadlessWebView::create_child(*this, traversable);

        return adopt_child_web_view(move(web_view)).handle();
    };

    on_reposition_window = [this](auto position) {
        m_previous_dimensions.set_location(position.template to_type<Web::DevicePixels>());
        page().async_set_window_position(position.template to_type<Web::DevicePixels>());
    };

    on_resize_window = [this](auto size) {
        m_viewport_size = size.template to_type<Web::DevicePixels>();

        page().async_set_window_size(m_viewport_size);
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

        page().async_set_window_position(screen_rect.location());
        page().async_set_window_size(screen_rect.size());
        handle_resize();
    };

    on_fullscreen_window = [this]() {
        m_previous_dimensions.set_size(m_viewport_size);
        m_viewport_size = screen_rect.size();

        page().async_set_window_position(screen_rect.location());
        page().async_set_window_size(screen_rect.size());
        set_is_fullscreen(Web::ViewportIsFullscreen::Yes);
    };

    on_exit_fullscreen_window = [this]() {
        m_viewport_size = m_previous_dimensions.size();

        page().async_set_window_position(m_previous_dimensions.location());
        page().async_set_window_size(m_previous_dimensions.size());
        set_is_fullscreen(Web::ViewportIsFullscreen::No);
    };

    on_request_alert = [this](auto const&) {
        m_pending_dialog = Web::PendingDialog::Alert;
    };

    on_request_confirm = [this](auto const&) {
        m_pending_dialog = Web::PendingDialog::Confirm;
    };

    on_request_prompt = [this](auto const&, auto const& prompt_text) {
        m_pending_dialog = Web::PendingDialog::Prompt;
        m_pending_prompt_text = prompt_text;
    };

    on_request_set_prompt_text = [this](auto const& prompt_text) {
        m_pending_prompt_text = prompt_text;
    };

    on_request_accept_dialog = [this]() {
        switch (m_pending_dialog) {
        case Web::PendingDialog::None:
            VERIFY_NOT_REACHED();
            break;
        case Web::PendingDialog::Alert:
            alert_closed();
            break;
        case Web::PendingDialog::Confirm:
            confirm_closed(true);
            break;
        case Web::PendingDialog::Prompt:
            prompt_closed(move(m_pending_prompt_text));
            break;
        }

        m_pending_dialog = Web::PendingDialog::None;
    };

    on_request_dismiss_dialog = [this]() {
        switch (m_pending_dialog) {
        case Web::PendingDialog::None:
            VERIFY_NOT_REACHED();
            break;
        case Web::PendingDialog::Alert:
            alert_closed();
            break;
        case Web::PendingDialog::Confirm:
            confirm_closed(false);
            break;
        case Web::PendingDialog::Prompt:
            prompt_closed({});
            break;
        }

        m_pending_dialog = Web::PendingDialog::None;
        m_pending_prompt_text.clear();
    };
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

HeadlessWebView& HeadlessWebView::adopt_child_web_view(NonnullOwnPtr<HeadlessWebView> web_view)
{
    auto* child_web_view = web_view.ptr();
    auto weak_this = make_weak_ptr<HeadlessWebView>();
    web_view->m_parent_web_view = weak_this;
    auto discard_child_web_view = [weak_this, child_web_view]() {
        if (weak_this)
            weak_this->discard_child_web_view(*child_web_view);
    };

    // Propagate crashes from child views to parent, so parent tests don't hang waiting for a child that crashed.
    web_view->on_web_content_crashed = [child_web_view, discard_child_web_view](auto crash_reason) {
        child_web_view->propagate_web_content_crash(crash_reason);
        discard_child_web_view();
    };
    web_view->on_close = move(discard_child_web_view);

    m_child_web_views.append(move(web_view));
    return *m_child_web_views.last();
}

// A headless browser has no tabs or windows, so what the browser UI would open in a new one gets a view of its own.
ViewImplementation* HeadlessWebView::create_view_for_new_tab_or_window(IsPrivate is_private)
{
    return &adopt_child_web_view(HeadlessWebView::create(m_theme, m_viewport_size, is_private));
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
            if (!weak_this || weak_this->handle().is_empty() || !weak_this->page().is_open())
                return;
            weak_this->force_close();
        });
    }

    if (!m_forced_close_timer->is_active())
        m_forced_close_timer->start();
}

void HeadlessWebView::prepare_page_for_tab(WebContentPage& page)
{
    ViewImplementation::prepare_page_for_tab(page);

    page.async_update_system_theme(m_theme);
    page.async_set_window_size(viewport_size());
    page.async_update_screen_rects({ { screen_rect } }, 0);
}

void HeadlessWebView::reset_viewport_size(Web::DevicePixelSize size)
{
    m_viewport_size = size;

    page().async_set_window_size(m_viewport_size);
    handle_resize();
}

void HeadlessWebView::update_zoom()
{
    ViewImplementation::update_zoom();
}

}
