/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/JsonObject.h>
#include <Hub/HubClientEndpoint.h>
#include <Hub/HubServerEndpoint.h>
#include <LibCore/GeolocationProvider.h>
#include <LibIPC/ConnectionToServer.h>
#include <LibWebView/GamepadSource.h>
#include <LibWebView/SpellChecker.h>

namespace Ladybird {

// The UI's end of its connection to the Hub.
class HubConnection final
    : public IPC::ConnectionToServer<HubClientEndpoint, HubServerEndpoint>
    , public HubClientEndpoint {
    C_OBJECT_ABSTRACT(HubConnection)

public:
    explicit HubConnection(NonnullOwnPtr<IPC::Transport>);
    virtual ~HubConnection() override;

    static HubConnection& the();
    static HubConnection* the_if_connected();

    // The Hub has answered initialize(), so from now on the UI cannot go on without it.
    void did_initialize();

    // The UI has no network of its own. The Hub makes the few requests it needs, each of a kind the Hub knows and checks.
    // These return an ID for stop_request().
    struct RequestResponse {
        bool had_network_error { false };
        Optional<u32> response_code;
        Vector<HTTP::Header> headers;
        ByteString body;
    };
    u64 fetch_search_suggestions(String engine_name, String query, WebView::IsPrivate, Function<void(RequestResponse)>);
    u64 post_report(URL::URL const&, Vector<HTTP::Header>, ByteString body, Function<void(RequestResponse)>);
    void stop_request(u64 request_id);

    void set_settings_from_hub(JsonValue const&);
    void send_settings_changes(JsonValue const&);

private:
    Core::GeolocationProvider* ensure_geolocation_provider();

    bool m_did_initialize { false };
    u64 m_next_request_id { 1 };
    HashMap<u64, Function<void(RequestResponse)>> m_requests;
    OwnPtr<Core::GeolocationProvider> m_geolocation_provider;
    HashMap<u64, Core::GeolocationProvider::RequestId> m_geolocation_requests;
    HashMap<u64, Core::GeolocationProvider::WatchId> m_geolocation_watches;
    HashMap<u64, WebView::Translator::SessionId> m_translator_sessions;

    WebView::SpellChecker::DocumentTag spell_document_tag(WebView::SpellChecker&, i64 document_tag);
    HashMap<i64, WebView::SpellChecker::DocumentTag> m_spell_document_tags;

    OwnPtr<WebView::GamepadSource> m_gamepad_source;

    // The Hub owns the settings. The settings here are its last word, plus the changes it has yet to confirm.
    struct SettingsChange {
        u64 id { 0 };
        JsonObject changes;
    };
    JsonObject m_last_known_settings;
    Vector<SettingsChange> m_unconfirmed_settings_changes;
    u64 m_next_settings_change_id { 1 };

    virtual void die() override;

    // ^HubClientEndpoint
    virtual void crash_reports_changed() override;
    virtual void request_finished(u64 request_id, bool had_network_error, Optional<u32> response_code, Vector<HTTP::Header> response_headers, ByteString body) override;
    virtual void app_open_new_tab(Vector<URL::URL> urls) override;
    virtual void app_open_new_window(Vector<URL::URL> urls) override;
    virtual void view_selected_text(u64 view_id, u64 request_id, Optional<ByteString> text) override;
    virtual void view_close_preflight_finished(u64 view_id, u64 request_id, bool may_close) override;
    virtual void view_page_dumps(u64 view_id, u64 request_id, Optional<String> dumps) override;
    virtual void pip_open(u64 view_id, u64 owner_view_id, Gfx::IntSize video_size) override;
    virtual void pip_hide(u64 view_id) override;
    virtual void pip_set_video_size(u64 view_id, Gfx::IntSize video_size) override;
    virtual void pip_close(u64 view_id) override;
    virtual void notification_show(u64 id, u64 page_id, Optional<u64> replaces, Web::NotificationsAPI::NotificationData data, String attribution, Optional<Gfx::ShareableBitmap> icon) override;
    virtual void notification_close(u64 id) override;
    virtual void geolocation_start() override;
    virtual void geolocation_request_position(u64 id) override;
    virtual void geolocation_cancel_position_request(u64 id) override;
    virtual void geolocation_start_watching(u64 id) override;
    virtual void geolocation_stop_watching(u64 id) override;
    virtual void translator_detect_language(u64 request_id, Web::TranslationLanguageSample sample) override;
    virtual void translator_target_languages(u64 request_id, String source_language, Vector<String> candidates) override;
    virtual void translator_create_session(u64 session_id, String source_language, String target_language, bool prefer_higher_quality) override;
    virtual void translator_choose_session_model_again(u64 session_id, bool prefer_higher_quality) override;
    virtual void translator_close_session(u64 session_id) override;
    virtual void translator_translate(u64 request_id, u64 session_id, Web::TranslationUnit unit) override;
    virtual void translator_open_language_download_settings() override;
    virtual void microphone_request_authorization() override;
    virtual void gamepad_start(bool ignore_system_gamepads) override;
    virtual void gamepad_set_polling(bool polling) override;
    virtual void gamepad_rumble(u32 handle, u16 low_frequency_magnitude, u16 high_frequency_magnitude, u32 duration_ms) override;
    virtual void gamepad_rumble_triggers(u32 handle, u16 left_magnitude, u16 right_magnitude, u32 duration_ms) override;
    virtual void spell_checker_close_document(i64 document_tag) override;
    virtual void spell_checker_check(u64 request_id, i64 document_tag, Utf16String text) override;
    virtual void spell_checker_guesses(u64 request_id, i64 document_tag, Utf16String word) override;
    virtual void spell_checker_ignore(i64 document_tag, Utf16String word) override;
    virtual void spell_checker_learn(i64 document_tag, Utf16String word) override;
    virtual void spell_checker_suggest(u64 request_id, i64 document_tag, Utf16String text, u32 caret_offset) override;
    virtual void app_focus_view_from_notification(u64 view_id) override;
    virtual void app_open_url_in_new_tab_from_notification(URL::URL url, Optional<u64> view_id, WebView::IsPrivate is_private) override;
    virtual void external_url_resolve_handler(u64 request_id, URL::URL url) override;
    virtual void external_url_launch(u64 handler_id, u64 request_id, URL::URL url) override;
    virtual void external_url_release_handler(u64 handler_id) override;
    virtual void download_added(Hub::DownloadState download) override;
    virtual void download_updated(Hub::DownloadState download) override;
    virtual void download_removed(u64 id, WebView::IsPrivate is_private) override;
    virtual void app_open_url_in_new_tab(URL::URL url, Web::HTML::ActivateTab activate_tab, Optional<u64> source_view_id, WebView::IsPrivate is_private) override;
    virtual void app_open_url_in_new_window(URL::URL url, WebView::IsPrivate is_private) override;
    virtual void app_open_navigation_in_new_tab(Web::HTML::PreparedNavigationDescriptor navigation, Web::HTML::ActivateTab activate_tab, Optional<u64> source_view_id, WebView::IsPrivate is_private) override;
    virtual void app_open_navigation_in_new_window(Web::HTML::PreparedNavigationDescriptor navigation, WebView::IsPrivate is_private) override;
    virtual void app_set_clipboard_text(String text, WebView::Application::ClipboardType type) override;
    virtual void app_insert_clipboard_item(Web::Clipboard::SystemClipboardItem item) override;
    virtual void app_update_tabs_display() override;
    virtual void app_did_clear_history_since(UnixDateTime since) override;
    virtual void app_quit_for_signal() override;
    virtual void app_did_enable_devtools() override;
    virtual void app_did_disable_devtools() override;
    virtual void app_open_urls_in_new_tabs(Vector<URL::URL> urls) override;
    virtual void app_activate_or_open_tab_with_url(URL::URL url) override;
    virtual void app_open_view_in_new_tab(u64 view_id, Optional<u64> source_view_id, WebView::IsPrivate is_private, Web::HTML::ActivateTab activate_tab) override;
    virtual void app_display_error_dialog(String message) override;
    virtual void app_display_download_confirmation_dialog(String download_name, ByteString path) override;
    virtual void app_read_clipboard_text(u64 request_id, WebView::Application::ClipboardType type) override;
    virtual void app_read_clipboard_item(u64 request_id) override;
    virtual void app_ask_user_for_download_path(u64 request_id, ByteString file) override;
    virtual void settings_changed(ByteString settings_json, u64 last_applied_change_id) override;
    virtual void web_ui_opened(u64 web_ui_id, String host, WebView::IsPrivate is_private) override;
    virtual void web_ui_message(u64 web_ui_id, String name, JsonValue data) override;
    virtual void web_ui_closed(u64 web_ui_id) override;
    virtual void view_request_context_menu(u64 view_id, WebView::ContextMenuRequest request) override;
    virtual void view_did_make_link_to_highlight(u64 view_id, Optional<URL::URL> url) override;
    virtual void view_session_history_changed(u64 view_id, WebView::SessionHistorySnapshot history, URL::URL url) override;
    virtual void view_visited_page(u64 view_id, URL::URL url, Optional<String> title, WebView::HistoryVisitTransition transition) override;
    virtual void view_visited_page_title_change(u64 view_id, URL::URL url, String title) override;
    virtual void view_favicon_change(u64 view_id, Optional<Gfx::ShareableBitmap> favicon) override;
    virtual void view_request_before_unload(u64 view_id, u64 answer_id, String source) override;
    virtual void view_request_external_url_confirmation(u64 view_id, u64 answer_id, URL::URL url, URL::Origin initiator_origin, String application_name) override;
    virtual void view_request_permission(u64 view_id, u64 answer_id, WebView::PermissionPromptRequest request) override;
    virtual void view_request_date_picker(u64 view_id, Utf16String input_type, Utf16String value, Utf16String minimum, Utf16String maximum) override;
    virtual void view_request_dictionary_lookup(u64 view_id, WebView::DictionaryLookup lookup, Gfx::IntPoint position) override;
    virtual void view_open_new_view(u64 view_id, u64 new_view_id, Hub::ViewState new_view_state, Web::HTML::ActivateTab activate_tab, Web::HTML::WebViewHints hints) override;
    virtual void view_update_zoom(u64 view_id) override;
    virtual void view_navigation_load_start(u64 view_id, URL::URL url) override;
    virtual void view_navigation_load_finish(u64 view_id, URL::URL url) override;
    virtual void view_install_backing_stores(u64 view_id, u64 page_id, Vector<i32> bitmap_ids, Vector<Gfx::SharedImage> backing_stores) override;
    virtual void view_add_backing_stores(u64 view_id, Vector<i32> bitmap_ids, Vector<Gfx::SharedImage> backing_stores) override;
    virtual void view_retire_backing_stores(u64 view_id, Vector<i32> bitmap_ids) override;
    virtual void view_present_backing_store(u64 view_id, u64 page_id, i32 bitmap_id, Gfx::IntSize size, double device_pixel_ratio) override;
    virtual void view_accept_presented_backing_store(u64 view_id, i32 bitmap_id, Gfx::IntRect damage_rect) override;
    virtual void view_keep_front_backing_store_as_backup(u64 view_id) override;
    virtual void view_clear_backup_backing_store(u64 view_id) override;
    virtual void view_clear_backing_stores(u64 view_id) override;
    virtual void view_finish_handling_key_event(u64 view_id, u64 input_id, Web::KeyEvent event) override;
    virtual void view_finish_handling_mouse_event(u64 view_id, u64 input_id, Web::MouseEvent event, Web::EventResult result) override;
    virtual void view_finish_handling_drag_event(u64 view_id, u64 input_id, Web::DragEvent event) override;
    virtual void view_release_input(u64 view_id, u64 input_id) override;
    virtual void view_state_changed(u64 view_id, Hub::ViewState state) override;
    virtual void view_load_start(u64 view_id) override;
    virtual void view_load_finish(u64 view_id, URL::URL url) override;
    virtual void view_activate_tab(u64 view_id) override;
    virtual void view_close(u64 view_id) override;
    virtual void view_link_hover(u64 view_id, URL::URL url) override;
    virtual void view_link_unhover(u64 view_id) override;
    virtual void view_title_change(u64 view_id, Utf16String title) override;
    virtual void view_url_change(u64 view_id, URL::URL url) override;
    virtual void view_loading_state_change(u64 view_id, bool is_loading) override;
    virtual void view_top_level_navigation_commit(u64 view_id) override;
    virtual void view_browser_history_traversal_complete(u64 view_id) override;
    virtual void view_cursor_change(u64 view_id, Gfx::Cursor cursor) override;
    virtual void view_input_method_state_change(u64 view_id) override;
    virtual void view_enter_tooltip_area(u64 view_id, ByteString tooltip) override;
    virtual void view_leave_tooltip_area(u64 view_id) override;
    virtual void view_content_blocking_change(u64 view_id, WebView::ContentBlockingSnapshot snapshot) override;
    virtual void view_request_alert(u64 view_id, Utf16String message) override;
    virtual void view_request_confirm(u64 view_id, Utf16String message) override;
    virtual void view_request_prompt(u64 view_id, Utf16String message, Utf16String default_value) override;
    virtual void view_request_set_prompt_text(u64 view_id, Utf16String message) override;
    virtual void view_before_browser_initiated_navigation(u64 view_id) override;
    virtual void view_request_accept_dialog(u64 view_id) override;
    virtual void view_request_dismiss_dialog(u64 view_id) override;
    virtual void view_request_website_issue_report(u64 view_id) override;
    virtual void view_request_screenshot(u64 view_id) override;
    virtual void view_accessibility_tree_received(u64 view_id, u64 sequence, Vector<WebView::AccessibilityNodeData> nodes) override;
    virtual void view_accessibility_tree_changed(u64 view_id, u64 base_sequence, u64 sequence, WebView::AccessibilityTreeDelta delta) override;
    virtual void view_accessibility_focus_changed(u64 view_id, i64 node_id) override;
    virtual void view_restore_window(u64 view_id) override;
    virtual void view_reposition_window(u64 view_id, Gfx::IntPoint position) override;
    virtual void view_resize_window(u64 view_id, Gfx::IntSize size) override;
    virtual void view_maximize_window(u64 view_id) override;
    virtual void view_minimize_window(u64 view_id) override;
    virtual void view_fullscreen_window(u64 view_id) override;
    virtual void view_exit_fullscreen_window(u64 view_id) override;
    virtual void view_request_color_picker(u64 view_id, Color current_color) override;
    virtual void view_cancel_permission_prompt(u64 view_id, WebView::PermissionPromptRequest request) override;
    virtual void view_tab_indicator_change(u64 view_id, WebView::TabIndicator indicator, bool visible) override;
    virtual void view_request_file_picker(u64 view_id, Web::HTML::FileFilter accepted_file_types, Web::HTML::AllowMultipleFiles allow_multiple_files) override;
    virtual void view_request_select_dropdown(u64 view_id, Gfx::IntPoint content_position, i32 minimum_width, Vector<Web::HTML::SelectItem> items) override;
    virtual void view_picture_in_picture_control_activated(u64 view_id, Compositing::PictureInPictureControl control) override;
    virtual void view_find_in_page(u64 view_id, size_t current_match_index, Optional<size_t> total_match_count) override;
    virtual void view_page_background_color_change(u64 view_id, Gfx::Color color) override;
    virtual void view_screen_wake_lock_state_changed(u64 view_id, Web::ScreenWakeLockState state) override;
    virtual void view_web_content_crashed(u64 view_id, WebView::ViewImplementation::WebContentCrashReason reason) override;
    virtual void view_crash_overlay_state_change(u64 view_id, bool active) override;
    virtual void view_crash_report_saved(u64 view_id) override;
    virtual void view_performance_stats(u64 view_id, WebView::TabPerformanceStats stats) override;
};

}
