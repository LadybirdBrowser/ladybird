/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <Hub/HubClientEndpoint.h>
#include <LibWebView/ViewImplementation.h>

namespace Hub {

// A tab's view. It holds the tab's pages and everything else about them, and leaves showing them to its ViewHost.
class View final : public WebView::ViewImplementation {
public:
    AK_ALLOC_WITH_KMALLOC;

    struct InitialState {
        WebView::IsPrivate is_private { WebView::IsPrivate::No };
        double device_pixel_ratio { 1.0 };
        double maximum_frames_per_second { 60.0 };
        Optional<u64> display_id;
        Core::AnonymousBuffer system_theme;
        Optional<ScreenRects> screen_rects;
        Optional<u64> owner_view_id;
        bool websites_prefer_dark { false };
        // The view of a tab can open others and ask the user about things, unlike that of a Picture-in-Picture window.
        bool is_tab { true };
    };

    explicit View(InitialState);

    void initialize(Optional<WebView::CanonicalTraversable&>);
    InitialState initial_state_for_related_view() const;

    ViewState current_state() const;

    // Tells the UI where the view has changed, which it must hear of before anything that makes it look.
    void push_state_to_ui();

    void did_get_answer(u64 answer_id, bool answer);
    void did_get_permission_answer(u64 answer_id, Web::Permissions::PromptOutcome);

    Gfx::IntSize screen_size() const;

    void set_viewport(Web::DevicePixelSize, double device_pixel_ratio);
    void set_system_theme(Core::AnonymousBuffer);
    void set_screen_rects(ScreenRects);
    bool scales_frames_to_fit() const { return m_scales_frames_to_fit; }

    using ViewImplementation::microphone_authorizer;
    using ViewImplementation::rect_fitting_frame;
    using ViewImplementation::set_page_background_color_to_system_canvas;

private:
    // ^WebView::ViewImplementation
    virtual void prepare_page_for_navigable(WebView::WebContentPage&) override;
    virtual Core::AnonymousBuffer system_theme() override { return m_system_theme; }
    virtual Optional<ScreenRects> screen_rects() const override { return m_screen_rects; }
    virtual void update_zoom() override;
    virtual void did_change_state() override { push_state_to_ui(); }
    virtual Web::DevicePixelSize viewport_size() const override { return m_viewport_size; }
    virtual Gfx::IntPoint to_content_position(Gfx::IntPoint widget_position) const override { return widget_position; }
    virtual Gfx::IntPoint to_widget_position(Gfx::IntPoint content_position) const override { return content_position; }
    virtual void did_request_context_menu(WebView::ContextMenuRequest) override;
    virtual void did_make_link_to_highlight(Optional<URL::URL>) override;
    virtual void did_change_session_history(WebView::SessionHistorySnapshot const&, URL::URL const&) override;
    virtual void did_visit_page(URL::URL const&, Optional<String> const& title, WebView::HistoryVisitTransition) override;
    virtual void did_change_visited_page_title(URL::URL const&, String const&) override;
    virtual void read_clipboard_item(Function<void(Web::Clipboard::SystemClipboardItem)>) const override;
    virtual void did_accept_presented_backing_store(i32 bitmap_id, Gfx::IntRect damage_rect) override;
    // NB: The UI hands a backing store back, once it no longer shows what is in it.
    virtual void release_backing_store(i32) override { }

    void forward_callbacks_to_ui(bool is_tab);

    Core::AnonymousBuffer m_system_theme;
    Optional<ScreenRects> m_screen_rects;
    Web::DevicePixelSize m_viewport_size;
    bool m_websites_prefer_dark { false };
    ByteBuffer m_last_state_pushed_to_ui;

    // What the view waits to hear from the user, through the UI.
    HashMap<u64, Function<void(bool)>> m_pending_answers;
    HashMap<u64, WebView::PermissionPromptCallback> m_pending_permission_answers;
    u64 m_next_answer_id { 1 };
};

}
