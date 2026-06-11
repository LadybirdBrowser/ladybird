/*
 * Copyright (c) 2020-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <LibGfx/Rect.h>
#include <LibWeb/CSS/StyleSheetIdentifier.h>
#include <LibWeb/HTML/AudioPlayState.h>
#include <LibWeb/HTML/FileFilter.h>
#include <LibWeb/HTML/SessionHistoryEntry.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/PixelUnits.h>
#include <LibWeb/StorageAPI/StorageEndpoint.h>
#include <LibWeb/WebDriver/Response.h>
#include <LibWebView/Forward.h>
#include <LibWebView/Mutation.h>
#include <LibWebView/StorageSetResult.h>
#include <WebContent/Forward.h>

namespace WebContent {

class PageClient final : public Web::PageClient {
    GC_CELL(PageClient, Web::PageClient);
    GC_DECLARE_ALLOCATOR(PageClient);

public:
    static GC::Ref<PageClient> create(JS::VM& vm, PageHost& page_host, u64 id);

    virtual ~PageClient() override;

    virtual u64 id() const override { return m_id; }

    virtual bool is_headless() const override;
    static void set_is_headless(bool);

    static void set_async_scrolling_enabled(bool);
    static void set_should_report_session_history_updates_in_test_mode(bool);

    virtual Web::Page& page() override { return *m_page; }
    virtual Web::Page const& page() const override { return *m_page; }
    virtual bool has_focus() const override { return m_has_focus; }

    ErrorOr<void> connect_to_webdriver(ByteString const& webdriver_endpoint);
    ErrorOr<void> connect_to_web_ui(IPC::TransportHandle);

    virtual Queue<Web::QueuedInputEvent>& input_event_queue() override;
    virtual void report_finished_handling_input_event(u64 page_id, Web::EventResult event_was_handled) override;
    virtual Web::Compositor::CompositorContextId allocate_compositor_context_id(Web::Compositor::PagePresentationRegistration) override;

    void set_palette_impl(Gfx::PaletteImpl&);
    void set_viewport(Web::DevicePixelSize const&, double device_pixel_ratio);
    void set_screen_rects(Vector<Web::DevicePixelRect> const& rects, size_t main_screen_index)
    {
        m_all_screen_rects = rects;
        m_main_screen_index = main_screen_index;
    }
    void set_zoom_level(double zoom_level);
    void set_maximum_frames_per_second(double maximum_frames_per_second);
    void set_preferred_color_scheme(Web::CSS::PreferredColorScheme);
    void set_preferred_contrast(Web::CSS::PreferredContrast);
    void set_preferred_motion(Web::CSS::PreferredMotion);
    void set_has_focus(bool);
    void set_window_handle(String);
    void did_start_webdriver_navigation(URL::URL const&);
    struct WebDriverHistoryTraversalResult {
        bool accepted { false };
        bool will_replace_web_content_process { false };
        bool will_change_top_level_entry { false };
    };
    void request_webdriver_history_traversal(int delta, Function<void(WebDriverHistoryTraversalResult)>);
    void did_complete_webdriver_history_traversal(u64 request_id, bool accepted, bool will_replace_web_content_process, bool will_change_top_level_entry);
    Web::WebDriver::Response request_webdriver_load_url_from_ui(URL::URL const&);
    Web::WebDriver::Response request_webdriver_traverse_history_from_ui(int delta);
    Web::WebDriver::Response request_webdriver_mark_web_content_session_history_stale();
    Web::WebDriver::Response request_webdriver_session_history();
    void set_is_scripting_enabled(bool);
    void set_window_position(Web::DevicePixelPoint);
    void set_window_size(Web::DevicePixelSize);
    void compositor_process_reconnected();
    void compositor_process_lost();

    void toggle_media_play_state();
    void toggle_media_mute_state();
    void toggle_media_loop_state();
    void toggle_media_controls_state();

    void alert_closed();
    void confirm_closed(bool accepted);
    void prompt_closed(Optional<String> response);
    void color_picker_update(Optional<Color> picked_color, Web::HTML::ColorPickerUpdateState state);
    void select_dropdown_closed(Optional<u32> const& selected_item_id);

    void set_user_style(String source);
    void did_connect_devtools_client();
    void did_disconnect_devtools_client();
    bool has_devtools_client() const { return m_devtools_client_count > 0; }
    virtual bool has_active_devtools_client() const override { return has_devtools_client(); }

    void initialize_js_console(Web::DOM::Document& document);
    void js_console_input(StringView js_source);
    void did_execute_js_console_input(JsonValue const&);
    void run_javascript(StringView js_source);
    void did_output_js_console_message(WebView::ConsoleOutput);
    void console_peer_did_misbehave(char const* reason);

    Vector<Web::CSS::StyleSheetIdentifier> list_style_sheets() const;

    virtual double zoom_level() const override { return m_zoom_level; }
    virtual double device_pixel_ratio() const override { return m_device_pixel_ratio; }
    virtual double device_pixels_per_css_pixel() const override { return m_device_pixel_ratio * m_zoom_level; }

    virtual bool supports_compositor() const override { return true; }
    virtual void ensure_compositor_host() override;
    virtual Web::Compositor::CompositorHost* compositor_host() override;
    virtual Web::Compositor::CompositorHost const* compositor_host() const override;

    void queue_screenshot_task(Optional<Web::UniqueNodeID> node_id);
    void send_current_needs_beforeunload_check();
    void wait_for_webdriver_navigation_completion(Optional<u64> page_load_timeout, Function<void(Web::WebDriver::Response)>);
    void did_complete_webdriver_navigation_completion(u64 request_id, Web::WebDriver::Response);
    void clear_pending_dom_mutations();
    void did_delete_all_cookies(u64 request_id);

private:
    struct PendingDOMMutation {
        GC::Ref<Web::DOM::Node> target;
        WebView::Mutation mutation;
    };

    PageClient(PageHost&, u64 id);

    virtual void visit_edges(JS::Cell::Visitor&) override;

    // ^PageClient
    virtual bool is_connection_open() const override;
    virtual bool is_url_suitable_for_same_process_navigation(URL::URL const& current_url, URL::URL const& target_url) const override;
    virtual void request_new_process_for_navigation(URL::URL const&, Variant<Empty, String, Web::HTML::POSTResource>, Web::Bindings::NavigationHistoryBehavior) override;
    virtual Gfx::Palette palette() const override;
    virtual Web::DevicePixelRect screen_rect() const override { return m_all_screen_rects[m_main_screen_index]; }
    virtual size_t screen_count() const override { return m_all_screen_rects.size(); }
    virtual Web::CSS::PreferredColorScheme preferred_color_scheme() const override { return m_preferred_color_scheme; }
    virtual Web::CSS::PreferredContrast preferred_contrast() const override { return m_preferred_contrast; }
    virtual Web::CSS::PreferredMotion preferred_motion() const override { return m_preferred_motion; }
    virtual void request_frame() override;
    virtual void page_did_request_cursor_change(Gfx::Cursor const&) override;
    virtual void page_did_change_title(Utf16String const&) override;
    virtual void page_did_change_url(URL::URL const&) override;
    virtual void page_did_request_refresh() override;
    virtual void page_did_request_resize_window(Gfx::IntSize) override;
    virtual void page_did_request_reposition_window(Gfx::IntPoint) override;
    virtual void page_did_request_restore_window() override;
    virtual void page_did_request_maximize_window() override;
    virtual void page_did_request_minimize_window() override;
    virtual void page_did_request_fullscreen_window() override;
    virtual void page_did_request_exit_fullscreen() override;
    virtual void page_did_request_tooltip_override(Web::CSSPixelPoint, ByteString const&) override;
    virtual void page_did_stop_tooltip_override() override;
    virtual void page_did_enter_tooltip_area(ByteString const&) override;
    virtual void page_did_leave_tooltip_area() override;
    virtual void page_did_hover_link(URL::URL const&) override;
    virtual void page_did_unhover_link() override;
    virtual void page_did_click_link(URL::URL const&, ByteString const& target, unsigned modifiers) override;
    virtual void page_did_middle_click_link(URL::URL const&, ByteString const& target, unsigned modifiers) override;
    virtual void page_did_request_context_menu(Web::CSSPixelPoint, Web::ContextMenuForInputEventsTarget) override;
    virtual void page_did_request_link_context_menu(Web::CSSPixelPoint, URL::URL const&, ByteString const& target, unsigned modifiers) override;
    virtual void page_did_request_image_context_menu(Web::CSSPixelPoint, URL::URL const&, ByteString const& target, unsigned modifiers, Optional<Gfx::Bitmap const*>) override;
    virtual void page_did_request_media_context_menu(Web::CSSPixelPoint, ByteString const& target, unsigned modifiers, Web::Page::MediaContextMenu const&) override;
    virtual void page_did_start_loading(URL::URL const&, Variant<Empty, String, Web::HTML::POSTResource>, bool, Web::Bindings::NavigationHistoryBehavior) override;
    virtual void page_did_cancel_loading(URL::URL const&) override;
    virtual void page_did_create_new_document(Web::DOM::Document&) override;
    virtual void page_did_change_active_document_in_top_level_browsing_context(Web::DOM::Document&) override;
    virtual void page_did_finish_loading(URL::URL const&) override;
    virtual void page_did_request_alert(String const&) override;
    virtual void page_did_request_confirm(String const&) override;
    virtual void page_did_request_prompt(String const&, String const&) override;
    virtual void page_did_request_set_prompt_text(String const&) override;
    virtual void page_did_request_accept_dialog() override;
    virtual void page_did_request_dismiss_dialog() override;
    virtual void page_did_change_favicon(Gfx::Bitmap const&) override;
    virtual Optional<Core::SharedVersion> page_did_request_document_cookie_version(Core::SharedVersionIndex document_index) override;
    virtual void page_did_receive_document_cookie_version_buffer(Core::AnonymousBuffer document_cookie_version_buffer) override;
    virtual void page_did_request_document_cookie_version_index(Web::UniqueNodeID document_id, String const& domain) override;
    virtual void page_did_receive_document_cookie_version_index(Web::UniqueNodeID document_id, Core::SharedVersionIndex document_index) override;
    virtual Vector<HTTP::Cookie::Cookie> page_did_request_all_cookies_webdriver(URL::URL const&) override;
    virtual Vector<HTTP::Cookie::Cookie> page_did_request_all_cookies_cookiestore(URL::URL const&) override;
    virtual Optional<HTTP::Cookie::Cookie> page_did_request_named_cookie(URL::URL const&, String const&) override;
    virtual HTTP::Cookie::VersionedCookie page_did_request_cookie(URL::URL const&, HTTP::Cookie::Source) override;
    virtual void page_did_set_cookie(URL::URL const&, HTTP::Cookie::ParsedCookie const&, HTTP::Cookie::Source) override;
    virtual void page_did_update_cookie(HTTP::Cookie::Cookie const&) override;
    virtual void page_did_expire_cookies_with_time_offset(AK::Duration) override;
    virtual void page_did_delete_all_cookies(URL::URL const&, GC::Ref<Web::WebIDL::Promise>) override;
    virtual void page_did_store_hsts_policy(String const&, HTTP::HSTS::ParsedHSTSPolicy const&) override;
    virtual bool page_did_is_known_hsts_host(String const&) override;
    virtual Optional<String> page_did_request_storage_item(Web::StorageAPI::StorageEndpointType storage_endpoint, String const& storage_key, String const& bottle_key) override;
    virtual WebView::StorageSetResult page_did_set_storage_item(Web::StorageAPI::StorageEndpointType storage_endpoint, String const& storage_key, String const& bottle_key, String const& value) override;
    virtual void page_did_remove_storage_item(Web::StorageAPI::StorageEndpointType storage_endpoint, String const& storage_key, String const& bottle_key) override;
    virtual Vector<String> page_did_request_storage_keys(Web::StorageAPI::StorageEndpointType storage_endpoint, String const& storage_key) override;
    virtual void page_did_clear_storage(Web::StorageAPI::StorageEndpointType storage_endpoint, String const& storage_key) override;
    virtual void page_did_broadcast_storage_change(Web::StorageAPI::StorageEndpointType storage_endpoint, String const& url, Optional<String> const& key, Optional<String> const& old_value, Optional<String> const& new_value) override;
    virtual void page_did_update_indexed_database(String const& url, Web::IndexedDB::TransactionChanges const&) override;
    virtual void page_did_update_resource_count(i32) override;
    virtual NewWebViewResult page_did_request_new_web_view(Web::HTML::ActivateTab, Web::HTML::WebViewHints, Web::HTML::TokenizedFeature::NoOpener) override;
    virtual void page_did_request_activate_tab() override;
    virtual void page_did_close_top_level_traversable() override;
    virtual void page_did_change_needs_beforeunload_check(bool needs_beforeunload_check) override;
    virtual void page_did_update_navigation_buttons_state(bool back_enabled, bool forward_enabled) override;
    virtual bool should_report_session_history_updates() const override;
    virtual void page_did_update_session_history(Vector<Web::HTML::SessionHistoryEntryDescriptor> const&, Vector<i32> const& used_steps, size_t current_used_step_index) override;
    virtual String page_did_request_ui_process_session_history_for_testing() override;
    virtual String page_did_update_session_history_and_request_ui_process_session_history_for_testing(Vector<Web::HTML::SessionHistoryEntryDescriptor> const&, Vector<i32> const& used_steps, size_t current_used_step_index) override;
    virtual bool page_did_request_traverse_the_history_by_delta(int delta, Web::HistoryTraversalPrecheck) override;
    virtual void request_file(Web::FileRequest) override;
    virtual void page_did_request_color_picker(Color current_color) override;
    virtual void page_did_request_file_picker(Web::HTML::FileFilter const& accepted_file_types, Web::HTML::AllowMultipleFiles) override;
    virtual void page_did_request_select_dropdown(Web::CSSPixelPoint content_position, Web::CSSPixels minimum_width, Vector<Web::HTML::SelectItem> items) override;
    virtual void page_did_finish_test(String const& text) override;
    virtual void page_did_set_test_timeout(double milliseconds) override;
    virtual void page_did_receive_reference_test_metadata(JsonValue) override;
    virtual void page_did_set_browser_zoom(double factor) override;
    virtual void page_did_set_device_pixel_ratio_for_testing(double ratio) override;
    virtual void page_did_change_theme_color(Gfx::Color color) override;
    virtual void page_did_change_background_color(Gfx::Color color) override;
    virtual void page_did_insert_clipboard_entry(Web::Clipboard::SystemClipboardRepresentation const&, StringView presentation_style) override;
    virtual void page_did_request_clipboard_entries(u64 request_id) override;
    virtual void page_did_request_primary_paste() override;
    virtual void page_did_update_primary_selection(String const&) override;
    virtual void page_did_change_audio_play_state(Web::HTML::AudioPlayState) override;
    virtual Web::HTML::WorkerAgentId start_worker_agent(Web::HTML::WorkerAgentStartRequest&&) override;
    virtual void close_worker_agent(Web::HTML::WorkerAgentId, Web::HTML::WorkerAgentOwnerToken) override;
    virtual void page_did_mutate_dom(FlyString const& type, Web::DOM::Node const& target, Web::DOM::NodeList& added_nodes, Web::DOM::NodeList& removed_nodes, GC::Ptr<Web::DOM::Node> previous_sibling, GC::Ptr<Web::DOM::Node> next_sibling, Optional<String> const& attribute_name) override;
    virtual void flush_pending_dom_mutations() override;
    virtual void page_did_take_screenshot(Gfx::ShareableBitmap const& screenshot) override;
    virtual void received_message_from_web_ui(String const& name, JS::Value data) override;
    virtual void page_did_start_network_request(u64 request_id, URL::URL const&, ByteString const&, Vector<HTTP::Header> const&, ReadonlyBytes, Optional<String>) override;
    virtual void page_did_receive_network_response_headers(u64 request_id, u32 status_code, Optional<String>, Vector<HTTP::Header> const&) override;
    virtual void page_did_receive_network_response_body(u64 request_id, ReadonlyBytes) override;
    virtual void page_did_finish_network_request(u64 request_id, u64 body_size, Requests::RequestTimingInfo const&, Optional<Requests::NetworkError> const&) override;
    virtual void page_did_post_broadcast_channel_message(Web::HTML::BroadcastChannelMessage const&) override;

    void setup_palette();
    ConnectionFromClient& client() const;
    void send_dom_mutation(Web::DOM::Node const& target, WebView::Mutation mutation);

    PageHost& m_owner;
    GC::Ref<Web::Page> m_page;
    RefPtr<Gfx::PaletteImpl> m_palette_impl;
    Vector<Web::DevicePixelRect> m_all_screen_rects { Web::DevicePixelRect {} };
    size_t m_main_screen_index { 0 };
    Web::DevicePixelSize m_viewport_size;
    double m_device_pixel_ratio { 1.0 };
    double m_zoom_level { 1.0 };
    double m_maximum_frames_per_second { 60.0 };
    u64 m_id { 0 };
    u64 m_next_delete_all_cookies_request_id { 1 };
    HashMap<u64, GC::Ref<Web::WebIDL::Promise>> m_pending_delete_all_cookies_promises;
    bool m_has_focus { true };

    Web::CSS::PreferredColorScheme m_preferred_color_scheme { Web::CSS::PreferredColorScheme::Auto };
    Web::CSS::PreferredContrast m_preferred_contrast { Web::CSS::PreferredContrast::NoPreference };
    Web::CSS::PreferredMotion m_preferred_motion { Web::CSS::PreferredMotion::NoPreference };

    Core::AnonymousBuffer m_document_cookie_version_buffer;

    u64 m_next_webdriver_navigation_completion_request_id { 0 };
    HashMap<u64, Function<void(Web::WebDriver::Response)>> m_pending_webdriver_navigation_completion_requests;
    u64 m_next_webdriver_history_traversal_request_id { 0 };
    HashMap<u64, Function<void(WebDriverHistoryTraversalResult)>> m_pending_webdriver_history_traversal_requests;

    RefPtr<WebDriverConnection> m_webdriver;
    RefPtr<WebUIConnection> m_web_ui;

    GC::Ptr<WebContentConsoleClient> m_top_level_document_console_client;

    RefPtr<Core::Timer> m_frame_timer;
    Optional<double> m_last_frame_dispatch_time;
    Queue<PendingDOMMutation> m_pending_dom_mutations;

    u64 m_devtools_client_count { 0 };
};

}
