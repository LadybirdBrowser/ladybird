/*
 * Copyright (c) 2020-2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/SourceLocation.h>
#include <LibIPC/ConnectionToServer.h>
#include <LibIPC/Transport.h>
#include <LibWeb/CSS/StyleSheetIdentifier.h>
#include <LibWeb/HTML/ActivateTab.h>
#include <LibWeb/HTML/FileFilter.h>
#include <LibWeb/HTML/SelectItem.h>
#include <LibWeb/HTML/WebViewHints.h>
#include <LibWeb/Page/EventResult.h>
#include <LibWebView/Forward.h>
#include <WebContent/WebContentClientEndpoint.h>
#include <WebContent/WebContentServerEndpoint.h>

namespace WebView {

class ViewImplementation;

class WebContentClient final
    : public IPC::ConnectionToServer<WebContentClientEndpoint, WebContentServerEndpoint>
    , public WebContentClientEndpoint {
    C_OBJECT_ABSTRACT(WebContentClient);

public:
    using InitTransport = Messages::WebContentServer::InitTransport;

    static Optional<ViewImplementation&> view_for_pid_and_page_id(pid_t pid, u64 page_id);

    template<CallableAs<IterationDecision, WebContentClient&> Callback>
    static void for_each_client(Callback callback);

    static size_t client_count() { return s_clients.size(); }

    explicit WebContentClient(IPC::Transport);
    WebContentClient(IPC::Transport, ViewImplementation&);
    ~WebContentClient();

    void assign_view(Badge<Application>, ViewImplementation&);
    void register_view(u64 page_id, ViewImplementation&);
    void unregister_view(u64 page_id);

    void web_ui_disconnected(Badge<WebUI>);

    Function<void()> on_web_content_process_crash;

    pid_t pid() const { return m_process_handle.pid; }
    void set_pid(pid_t pid) { m_process_handle.pid = pid; }

private:
    virtual void die() override;

    virtual void did_paint(u64 page_id, Gfx::IntRect, i32) override;
    virtual void did_request_new_process_for_navigation(u64 page_id, URL::URL url) override;
    virtual void did_finish_loading(u64 page_id, URL::URL) override;
    virtual void did_request_refresh(u64 page_id) override;
    virtual void did_request_cursor_change(u64 page_id, Gfx::Cursor) override;
    virtual void did_change_title(u64 page_id, ByteString) override;
    virtual void did_change_url(u64 page_id, URL::URL) override;
    virtual void did_request_tooltip_override(u64 page_id, Gfx::IntPoint, ByteString) override;
    virtual void did_stop_tooltip_override(u64 page_id) override;
    virtual void did_enter_tooltip_area(u64 page_id, ByteString) override;
    virtual void did_leave_tooltip_area(u64 page_id) override;
    virtual void did_hover_link(u64 page_id, URL::URL) override;
    virtual void did_unhover_link(u64 page_id) override;
    virtual void did_click_link(u64 page_id, URL::URL, ByteString, unsigned) override;
    virtual void did_middle_click_link(u64 page_id, URL::URL, ByteString, unsigned) override;
    virtual void did_start_loading(u64 page_id, URL::URL, bool) override;
    virtual void did_request_context_menu(u64 page_id, Gfx::IntPoint) override;
    virtual void did_request_link_context_menu(u64 page_id, Gfx::IntPoint, URL::URL, ByteString, unsigned) override;
    virtual void did_request_image_context_menu(u64 page_id, Gfx::IntPoint, URL::URL, ByteString, unsigned, Optional<Gfx::ShareableBitmap>) override;
    virtual void did_request_media_context_menu(u64 page_id, Gfx::IntPoint, ByteString, unsigned, Web::Page::MediaContextMenu) override;
    virtual void did_get_source(u64 page_id, URL::URL, URL::URL, String) override;
    virtual void did_inspect_dom_tree(u64 page_id, String) override;
    virtual void did_inspect_dom_node(u64 page_id, DOMNodeProperties) override;
    virtual void did_inspect_accessibility_tree(u64 page_id, String) override;
    virtual void did_get_hovered_node_id(u64 page_id, Web::UniqueNodeID node_id) override;
    virtual void did_finish_editing_dom_node(u64 page_id, Optional<Web::UniqueNodeID> node_id) override;
    virtual void did_mutate_dom(u64 page_id, Mutation) override;
    virtual void did_get_dom_node_html(u64 page_id, String html) override;
    virtual void did_list_style_sheets(u64 page_id, Vector<Web::CSS::StyleSheetIdentifier> stylesheets) override;
    virtual void did_get_style_sheet_source(u64 page_id, Web::CSS::StyleSheetIdentifier identifier, URL::URL, String source) override;
    virtual void did_take_screenshot(u64 page_id, Gfx::ShareableBitmap screenshot) override;
    virtual void did_get_internal_page_info(u64 page_id, PageInfoType, String) override;
    virtual void did_execute_js_console_input(u64 page_id, JsonValue) override;
    virtual void did_output_js_console_message(u64 page_id, i32 message_index) override;
    virtual void did_get_js_console_messages(u64 page_id, i32 start_index, Vector<ConsoleOutput>) override;
    virtual void did_change_favicon(u64 page_id, Gfx::ShareableBitmap) override;
    virtual void did_request_alert(u64 page_id, String) override;
    virtual void did_request_confirm(u64 page_id, String) override;
    virtual void did_request_prompt(u64 page_id, String, String) override;
    virtual void did_request_set_prompt_text(u64 page_id, String message) override;
    virtual void did_request_accept_dialog(u64 page_id) override;
    virtual void did_request_dismiss_dialog(u64 page_id) override;
    virtual Messages::WebContentClient::DidRequestAllCookiesResponse did_request_all_cookies(URL::URL) override;
    virtual Messages::WebContentClient::DidRequestNamedCookieResponse did_request_named_cookie(URL::URL, String) override;
    virtual Messages::WebContentClient::DidRequestCookieResponse did_request_cookie(URL::URL, Web::Cookie::Source) override;
    virtual void did_set_cookie(URL::URL, Web::Cookie::ParsedCookie, Web::Cookie::Source) override;
    virtual void did_update_cookie(Web::Cookie::Cookie) override;
    virtual void did_expire_cookies_with_time_offset(AK::Duration) override;
    virtual Messages::WebContentClient::DidRequestNewWebViewResponse did_request_new_web_view(u64 page_id, Web::HTML::ActivateTab, Web::HTML::WebViewHints, Optional<u64> page_index) override;
    virtual void did_request_activate_tab(u64 page_id) override;
    virtual void did_close_browsing_context(u64 page_id) override;
    virtual void did_update_resource_count(u64 page_id, i32 count_waiting) override;
    virtual void did_request_restore_window(u64 page_id) override;
    virtual void did_request_reposition_window(u64 page_id, Gfx::IntPoint) override;
    virtual void did_request_resize_window(u64 page_id, Gfx::IntSize) override;
    virtual void did_request_maximize_window(u64 page_id) override;
    virtual void did_request_minimize_window(u64 page_id) override;
    virtual void did_request_fullscreen_window(u64 page_id) override;
    virtual void did_request_file(u64 page_id, ByteString path, i32) override;
    virtual void did_request_color_picker(u64 page_id, Color current_color) override;
    virtual void did_request_file_picker(u64 page_id, Web::HTML::FileFilter accepted_file_types, Web::HTML::AllowMultipleFiles) override;
    virtual void did_request_select_dropdown(u64 page_id, Gfx::IntPoint content_position, i32 minimum_width, Vector<Web::HTML::SelectItem> items) override;
    virtual void did_finish_handling_input_event(u64 page_id, Web::EventResult event_result) override;
    virtual void did_finish_test(u64 page_id, String text) override;
    virtual void did_set_test_timeout(u64 page_id, double milliseconds) override;
    virtual void did_set_browser_zoom(u64 page_id, double factor) override;
    virtual void did_find_in_page(u64 page_id, size_t current_match_index, Optional<size_t> total_match_count) override;
    virtual void did_change_theme_color(u64 page_id, Gfx::Color color) override;
    virtual void did_insert_clipboard_entry(u64 page_id, String data, String presentation_style, String mime_type) override;
    virtual void did_change_audio_play_state(u64 page_id, Web::HTML::AudioPlayState) override;
    virtual void did_update_navigation_buttons_state(u64 page_id, bool back_enabled, bool forward_enabled) override;
    virtual void did_allocate_backing_stores(u64 page_id, i32 front_bitmap_id, Gfx::ShareableBitmap, i32 back_bitmap_id, Gfx::ShareableBitmap) override;
    virtual Messages::WebContentClient::RequestWorkerAgentResponse request_worker_agent(u64 page_id) override;

    Optional<ViewImplementation&> view_for_page_id(u64, SourceLocation = SourceLocation::current());

    // FIXME: Does a HashMap holding references make sense?
    HashMap<u64, ViewImplementation*> m_views;

    ProcessHandle m_process_handle;

    RefPtr<WebUI> m_web_ui;

    static HashTable<WebContentClient*> s_clients;
};

template<CallableAs<IterationDecision, WebContentClient&> Callback>
void WebContentClient::for_each_client(Callback callback)
{
    for (auto& it : s_clients) {
        if (callback(*it) == IterationDecision::Break)
            return;
    }
}

}
