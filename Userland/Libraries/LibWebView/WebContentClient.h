/*
 * Copyright (c) 2020-2021, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/SourceLocation.h>
#include <LibIPC/ConnectionToServer.h>
#include <LibWeb/HTML/ActivateTab.h>
#include <LibWeb/HTML/FileFilter.h>
#include <LibWeb/HTML/SelectItem.h>
#include <LibWeb/HTML/WebViewHints.h>
#include <WebContent/WebContentClientEndpoint.h>
#include <WebContent/WebContentServerEndpoint.h>

namespace WebView {

class ViewImplementation;

class WebContentClient final
    : public IPC::ConnectionToServer<WebContentClientEndpoint, WebContentServerEndpoint>
    , public WebContentClientEndpoint {
    IPC_CLIENT_CONNECTION(WebContentClient, "/tmp/session/%sid/portal/webcontent"sv);

public:
    static Optional<ViewImplementation&> view_for_pid_and_page_id(pid_t pid, u64 page_id);

    template<CallableAs<IterationDecision, WebContentClient&> Callback>
    static void for_each_client(Callback callback);

    static size_t client_count() { return s_clients.size(); }

    WebContentClient(NonnullOwnPtr<Core::LocalSocket>, ViewImplementation&);
    ~WebContentClient();

    void register_view(u64 page_id, ViewImplementation&);
    void unregister_view(u64 page_id);

    Function<void()> on_web_content_process_crash;

    void set_pid(pid_t pid) { m_process_handle.pid = pid; }

private:
    virtual void die() override;

    virtual void did_paint(u64 page_id, Gfx::IntRect const&, i32) override;
    virtual void did_finish_loading(u64 page_id, URL::URL const&) override;
    virtual void did_request_navigate_back(u64 page_id) override;
    virtual void did_request_navigate_forward(u64 page_id) override;
    virtual void did_request_refresh(u64 page_id) override;
    virtual void did_request_cursor_change(u64 page_id, i32) override;
    virtual void did_layout(u64 page_id, Gfx::IntSize) override;
    virtual void did_change_title(u64 page_id, ByteString const&) override;
    virtual void did_change_url(u64 page_id, URL::URL const&) override;
    virtual void did_request_tooltip_override(u64 page_id, Gfx::IntPoint, ByteString const&) override;
    virtual void did_stop_tooltip_override(u64 page_id) override;
    virtual void did_enter_tooltip_area(u64 page_id, ByteString const&) override;
    virtual void did_leave_tooltip_area(u64 page_id) override;
    virtual void did_hover_link(u64 page_id, URL::URL const&) override;
    virtual void did_unhover_link(u64 page_id) override;
    virtual void did_click_link(u64 page_id, URL::URL const&, ByteString const&, unsigned) override;
    virtual void did_middle_click_link(u64 page_id, URL::URL const&, ByteString const&, unsigned) override;
    virtual void did_start_loading(u64 page_id, URL::URL const&, bool) override;
    virtual void did_request_context_menu(u64 page_id, Gfx::IntPoint) override;
    virtual void did_request_link_context_menu(u64 page_id, Gfx::IntPoint, URL::URL const&, ByteString const&, unsigned) override;
    virtual void did_request_image_context_menu(u64 page_id, Gfx::IntPoint, URL::URL const&, ByteString const&, unsigned, Gfx::ShareableBitmap const&) override;
    virtual void did_request_media_context_menu(u64 page_id, Gfx::IntPoint, ByteString const&, unsigned, Web::Page::MediaContextMenu const&) override;
    virtual void did_get_source(u64 page_id, URL::URL const&, ByteString const&) override;
    virtual void did_inspect_dom_tree(u64 page_id, ByteString const&) override;
    virtual void did_inspect_dom_node(u64 page_id, bool has_style, ByteString const& computed_style, ByteString const& resolved_style, ByteString const& custom_properties, ByteString const& node_box_sizing, ByteString const& aria_properties_state) override;
    virtual void did_inspect_accessibility_tree(u64 page_id, ByteString const&) override;
    virtual void did_get_hovered_node_id(u64 page_id, i32 node_id) override;
    virtual void did_finish_editing_dom_node(u64 page_id, Optional<i32> const& node_id) override;
    virtual void did_get_dom_node_html(u64 page_id, String const& html) override;
    virtual void did_take_screenshot(u64 page_id, Gfx::ShareableBitmap const& screenshot) override;
    virtual void did_output_js_console_message(u64 page_id, i32 message_index) override;
    virtual void did_get_js_console_messages(u64 page_id, i32 start_index, Vector<ByteString> const& message_types, Vector<ByteString> const& messages) override;
    virtual void did_change_favicon(u64 page_id, Gfx::ShareableBitmap const&) override;
    virtual void did_request_alert(u64 page_id, String const&) override;
    virtual void did_request_confirm(u64 page_id, String const&) override;
    virtual void did_request_prompt(u64 page_id, String const&, String const&) override;
    virtual void did_request_set_prompt_text(u64 page_id, String const& message) override;
    virtual void did_request_accept_dialog(u64 page_id) override;
    virtual void did_request_dismiss_dialog(u64 page_id) override;
    virtual Messages::WebContentClient::DidRequestAllCookiesResponse did_request_all_cookies(u64 page_id, URL::URL const&) override;
    virtual Messages::WebContentClient::DidRequestNamedCookieResponse did_request_named_cookie(u64 page_id, URL::URL const&, String const&) override;
    virtual Messages::WebContentClient::DidRequestCookieResponse did_request_cookie(u64 page_id, URL::URL const&, Web::Cookie::Source) override;
    virtual void did_set_cookie(u64 page_id, URL::URL const&, Web::Cookie::ParsedCookie const&, Web::Cookie::Source) override;
    virtual void did_update_cookie(u64 page_id, Web::Cookie::Cookie const&) override;
    virtual Messages::WebContentClient::DidRequestNewWebViewResponse did_request_new_web_view(u64 page_id, Web::HTML::ActivateTab const&, Web::HTML::WebViewHints const&, Optional<u64> const& page_index) override;
    virtual void did_request_activate_tab(u64 page_id) override;
    virtual void did_close_browsing_context(u64 page_id) override;
    virtual void did_update_resource_count(u64 page_id, i32 count_waiting) override;
    virtual void did_request_restore_window(u64 page_id) override;
    virtual Messages::WebContentClient::DidRequestRepositionWindowResponse did_request_reposition_window(u64 page_id, Gfx::IntPoint) override;
    virtual Messages::WebContentClient::DidRequestResizeWindowResponse did_request_resize_window(u64 page_id, Gfx::IntSize) override;
    virtual Messages::WebContentClient::DidRequestMaximizeWindowResponse did_request_maximize_window(u64 page_id) override;
    virtual Messages::WebContentClient::DidRequestMinimizeWindowResponse did_request_minimize_window(u64 page_id) override;
    virtual Messages::WebContentClient::DidRequestFullscreenWindowResponse did_request_fullscreen_window(u64 page_id) override;
    virtual void did_request_file(u64 page_id, ByteString const& path, i32) override;
    virtual void did_request_color_picker(u64 page_id, Color const& current_color) override;
    virtual void did_request_file_picker(u64 page_id, Web::HTML::FileFilter const& accepted_file_types, Web::HTML::AllowMultipleFiles) override;
    virtual void did_request_select_dropdown(u64 page_id, Gfx::IntPoint content_position, i32 minimum_width, Vector<Web::HTML::SelectItem> const& items) override;
    virtual void did_finish_handling_input_event(u64 page_id, bool event_was_accepted) override;
    virtual void did_finish_text_test(u64 page_id) override;
    virtual void did_find_in_page(u64 page_id, size_t current_match_index, Optional<size_t> const& total_match_count) override;
    virtual void did_change_theme_color(u64 page_id, Gfx::Color color) override;
    virtual void did_insert_clipboard_entry(u64 page_id, String const& data, String const& presentation_style, String const& mime_type) override;
    virtual void did_change_audio_play_state(u64 page_id, Web::HTML::AudioPlayState) override;
    virtual void did_update_navigation_buttons_state(u64 page_id, bool back_enabled, bool forward_enabled) override;
    virtual void did_allocate_backing_stores(u64 page_id, i32 front_bitmap_id, Gfx::ShareableBitmap const&, i32 back_bitmap_id, Gfx::ShareableBitmap const&) override;
    virtual void inspector_did_load(u64 page_id) override;
    virtual void inspector_did_select_dom_node(u64 page_id, i32 node_id, Optional<Web::CSS::Selector::PseudoElement::Type> const& pseudo_element) override;
    virtual void inspector_did_set_dom_node_text(u64 page_id, i32 node_id, String const& text) override;
    virtual void inspector_did_set_dom_node_tag(u64 page_id, i32 node_id, String const& tag) override;
    virtual void inspector_did_add_dom_node_attributes(u64 page_id, i32 node_id, Vector<Attribute> const& attributes) override;
    virtual void inspector_did_replace_dom_node_attribute(u64 page_id, i32 node_id, size_t attribute_index, Vector<Attribute> const& replacement_attributes) override;
    virtual void inspector_did_request_dom_tree_context_menu(u64 page_id, i32 node_id, Gfx::IntPoint position, String const& type, Optional<String> const& tag, Optional<size_t> const& attribute_index) override;
    virtual void inspector_did_execute_console_script(u64 page_id, String const& script) override;
    virtual Messages::WebContentClient::RequestWorkerAgentResponse request_worker_agent(u64 page_id) override;

    Optional<ViewImplementation&> view_for_page_id(u64, SourceLocation = SourceLocation::current());

    // FIXME: Does a HashMap holding references make sense?
    HashMap<u64, ViewImplementation*> m_views;

    ProcessHandle m_process_handle;

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
