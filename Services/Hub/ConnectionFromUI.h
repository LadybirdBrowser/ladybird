/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <Hub/HubClientEndpoint.h>
#include <Hub/HubServerEndpoint.h>
#include <Hub/View.h>
#include <LibIPC/ConnectionFromClient.h>
#include <LibRequests/Request.h>
#include <LibWebView/CrashReportOutbox.h>
#include <LibWebView/FileDownloader.h>

namespace Hub {

// The Hub's end of its connection to the UI.
class ConnectionFromUI final
    : public IPC::ConnectionFromClient<HubClientEndpoint, HubServerEndpoint>
    , public WebView::FileDownloaderObserver
    , public WebView::CrashReportOutboxObserver {
    C_OBJECT(ConnectionFromUI);

public:
    virtual ~ConnectionFromUI() override;

    static ConnectionFromUI* the();

    static Vector<DownloadState> downloads();

    // The ID that the UI gave an input event, from the browser data of the event.
    static u64 input_id_of(Web::BrowserInputData const&);

    virtual void die() override;

private:
    explicit ConnectionFromUI(NonnullOwnPtr<IPC::Transport>);

    // ^WebView::CrashReportOutboxObserver
    virtual void crash_reports_changed() override;

    static View* find_view(u64 view_id);

    // The requests that the UI has asked us to make, as it has no network of its own.
    HashMap<u64, NonnullRefPtr<Requests::Request>> m_requests;
    u64 m_last_applied_settings_change_id { 0 };

    // A close that the UI got ready for, and finishes once the tab is gone from its window.
    HashMap<u64, Function<void()>> m_prepared_closes;

    // ^WebView::FileDownloaderObserver
    virtual void download_added(WebView::FileDownloader::Download const&) override;
    virtual void download_updated(WebView::FileDownloader::Download const&) override;
    virtual void download_removed(u64 id, WebView::IsPrivate) override;

    // ^HubServerEndpoint
    virtual Messages::HubServer::InitializeResponse initialize() override;
    virtual Messages::HubServer::CreateViewResponse create_view(Hub::NewViewRequest request) override;
    virtual Messages::HubServer::ViewDescriptionResponse view_description(u64 view_id) override;
    virtual void ui_did_start() override;
    virtual void add_web_ui_page(String host, String title) override;
    virtual void send_web_ui_message(u64 web_ui_id, String name, JsonValue data) override;
    virtual Messages::HubServer::WebUiConsumeUserActivationResponse web_ui_consume_user_activation(u64 web_ui_id) override;
    virtual void fetch_search_suggestions(u64 request_id, String engine_name, String query, WebView::IsPrivate is_private) override;
    virtual void post_report(u64 request_id, URL::URL url, Vector<HTTP::Header> headers, ByteString body) override;
    void start_request(u64 request_id, ByteString const& method, URL::URL const&, HTTP::HeaderList const& headers, ByteString const& body, HTTP::CacheMode, HTTP::Cookie::IncludeCredentials, WebView::IsPrivate, ReadonlySpan<StringView> response_header_names);
    virtual void stop_request(u64 request_id) override;
    virtual void toggle_devtools_enabled() override;
    virtual void set_preferred_contrast(Web::CSS::PreferredContrast) override;
    virtual void set_preferred_motion(Web::CSS::PreferredMotion) override;
    virtual void set_user_agent_string(String user_agent) override;
    virtual void set_navigator_compatibility_mode(Web::NavigatorCompatibilityMode) override;
    virtual void set_scripting_enabled(bool enabled) override;
    virtual void set_blocks_pop_ups(bool blocks) override;
    virtual void set_shows_line_box_borders(bool shows) override;
    virtual void set_shows_caret_hit_test_debug_overlay(bool shows) override;
    virtual void dump_cookies() override;
    virtual void crash_compositor_process() override;
    virtual void launch_devtools_client() override;
    virtual void reset_private_browsing_session() override;
    virtual void notify_compositor_gpu_presentation_unavailable() override;
    virtual void ui_info_changed(Hub::UIInfo info) override;
    virtual Messages::HubServer::ProcessSnapshotsResponse process_snapshots() override;
    virtual Messages::HubServer::TerminateProcessResponse terminate_process(u64 child_id) override;
    virtual Messages::HubServer::ViewNeedsBeforeunloadCheckResponse view_needs_beforeunload_check(u64 view_id) override;
    virtual Messages::HubServer::ViewSessionHistoryTraversalItemsResponse view_session_history_traversal_items(u64 view_id, i32 direction) override;
    virtual Messages::HubServer::ViewSessionHistorySnapshotResponse view_session_history_snapshot(u64 view_id) override;
    virtual Messages::HubServer::ViewRestoreSessionHistoryFromSnapshotResponse view_restore_session_history_from_snapshot(u64 view_id, WebView::SessionHistorySnapshot snapshot) override;
    virtual Messages::HubServer::ViewHistoryStepAtDeltaResponse view_history_step_at_delta(u64 view_id, i32 delta) override;
    virtual Messages::HubServer::ViewLookUpSelectedTextAtResponse view_look_up_selected_text_at(u64 view_id, Gfx::IntPoint position) override;
    virtual Messages::HubServer::ViewPageDescriptionResponse view_page_description(u64 view_id) override;
    virtual void view_set_site_permission(u64 view_id, URL::Origin origin, Web::Permissions::PermissionName name, Optional<WebView::PermissionDecision> decision) override;
    virtual void view_did_show_permission_prompt(u64 view_id, WebView::PermissionPromptRequest request) override;
    virtual void view_load_navigation(u64 view_id, Web::HTML::PreparedNavigationDescriptor navigation) override;
    virtual void view_open_navigation_in_new_tab(u64 view_id, Web::HTML::PreparedNavigationDescriptor navigation, Web::HTML::ActivateTab activate_tab) override;
    virtual void view_open_navigation_in_new_window(u64 view_id, Web::HTML::PreparedNavigationDescriptor navigation, WebView::IsPrivate is_private) override;
    virtual void view_traverse_the_history_to_step(u64 view_id, i32 step) override;
    virtual void view_set_reports_session_history(u64 view_id, bool reports_session_history) override;
    virtual void view_file_picker_closed(u64 view_id, Vector<Web::HTML::SelectedFile> selected_files) override;
    virtual void view_save_screenshot(u64 view_id, Gfx::ShareableBitmap bitmap) override;
    virtual void view_request_selected_text(u64 view_id, u64 request_id) override;
    virtual void view_request_close_preflight(u64 view_id, u64 request_id) override;
    virtual void view_request_page_dumps(u64 view_id, u64 request_id) override;
    virtual void view_prepare_for_immediate_close(u64 view_id) override;
    virtual void view_prepare_for_close_without_prompting(u64 view_id, u64 close_id) override;
    virtual void finish_prepared_close(u64 close_id) override;
    virtual void pip_did_resize(u64 view_id, Gfx::IntSize size) override;
    virtual void pip_did_close(u64 view_id, bool video_keeps_playing) override;
    virtual void notification_shown(u64 id) override;
    virtual void notification_failed(u64 id) override;
    virtual void notification_clicked(u64 id) override;
    virtual void notification_closed(u64 id, bool by_user) override;
    virtual void geolocation_position(u64 id, Core::GeolocationCoordinates coordinates) override;
    virtual void geolocation_error(u64 id, Core::GeolocationError error) override;
    virtual void geolocation_authorization_changed(Core::GeolocationAuthorization authorization) override;
    virtual void microphone_authorization_changed(WebView::MicrophoneAuthorization authorization) override;
    virtual void gamepad_connected(Web::Gamepad::GamepadDescription description, Web::Gamepad::GamepadState state) override;
    virtual void gamepad_disconnected(u32 handle) override;
    virtual void gamepad_states_changed(Vector<Web::Gamepad::GamepadState> states) override;
    virtual void system_time_zone_changed() override;
    virtual void translator_language_detected(u64 request_id, Optional<String> language) override;
    virtual void translator_target_languages_ready(u64 request_id, Vector<WebView::Translator::TargetLanguage> target_languages) override;
    virtual void translator_translation_finished(u64 request_id, Optional<Vector<Web::TranslationRun>> runs) override;
    virtual void translator_languages_changed() override;
    virtual void spell_checker_checked(u64 request_id, Vector<Web::SpellingError> errors) override;
    virtual void spell_checker_guessed(u64 request_id, Vector<Utf16String> guesses) override;
    virtual void spell_checker_suggested(u64 request_id, Optional<Web::WritingSuggestion> suggestion) override;
    virtual void external_url_handler_resolved(u64 request_id, Optional<u64> handler_id, String application_name, bool is_ladybird) override;
    virtual void external_url_launched(u64 request_id, bool launched) override;
    virtual void cancel_download(u64 id) override;
    virtual void pause_download(u64 id) override;
    virtual void resume_download(u64 id) override;
    virtual void prune_inactive_downloads(WebView::IsPrivate is_private) override;
    virtual void pause_active_downloads() override;
    virtual void cancel_unresumable_downloads() override;
    virtual void active_view_changed(Optional<u64> view_id) override;
    virtual void answer_clipboard_text(u64 request_id, Utf16String text) override;
    virtual void answer_clipboard_item(u64 request_id, Web::Clipboard::SystemClipboardItem item) override;
    virtual void answer_download_path(u64 request_id, Optional<ByteString> path) override;
    virtual void update_settings(u64 change_id, ByteString changes_json) override;
    virtual void view_answer(u64 view_id, u64 answer_id, bool answer) override;
    virtual void view_answer_permission_request(u64 view_id, u64 answer_id, Web::Permissions::PromptOutcome outcome) override;
    virtual void destroy_view(u64 view_id) override;
    virtual void view_set_viewport(u64 view_id, Web::DevicePixelSize size, double device_pixel_ratio) override;
    virtual void view_set_system_theme(u64 view_id, Core::AnonymousBuffer theme) override;
    virtual void view_set_screen_rects(u64 view_id, WebView::ViewImplementation::ScreenRects screen_rects) override;
    virtual void view_set_page_background_color_to_system_canvas(u64 view_id, bool dark) override;
    virtual void release_backing_store(u64 page_id, i32 bitmap_id) override;
    virtual void view_enqueue_key_event(u64 view_id, u64 input_id, Web::KeyEvent event) override;
    virtual void view_enqueue_mouse_event(u64 view_id, u64 input_id, Web::MouseEvent event) override;
    virtual void view_enqueue_drag_event(u64 view_id, u64 input_id, Web::DragEvent event) override;
    virtual void view_enqueue_pinch_event(u64 view_id, Web::PinchEvent event) override;
    virtual void view_load(u64 view_id, URL::URL url, Web::Bindings::NavigationHistoryBehavior history_behavior) override;
    virtual void view_load_html(u64 view_id, ByteString html) override;
    virtual void view_find_in_page(u64 view_id, Utf16String query, AK::CaseSensitivity case_sensitivity, bool highlight_all_matches) override;
    virtual void view_set_window_position(u64 view_id, Gfx::IntPoint position) override;
    virtual void view_set_window_size(u64 view_id, Gfx::IntSize size) override;
    virtual void view_set_system_visibility_state(u64 view_id, Web::HTML::VisibilityState visibility_state) override;
    virtual void view_set_presents_from_cpu_memory(u64 view_id, bool presents_from_cpu_memory) override;
    virtual void view_set_has_system_focus(u64 view_id, bool has_system_focus) override;
    virtual void view_open_url_in_new_tab(u64 view_id, URL::URL url, Web::HTML::ActivateTab activate_tab) override;
    virtual void view_open_url_in_new_window(u64 view_id, URL::URL url, WebView::IsPrivate is_private) override;
    virtual void view_set_next_history_visit_transition(u64 view_id, WebView::HistoryVisitTransition transition) override;
    virtual void view_reload(u64 view_id, WebView::BypassCache bypass_cache) override;
    virtual void view_stop_loading(u64 view_id) override;
    virtual void view_undo(u64 view_id) override;
    virtual void view_redo(u64 view_id) override;
    virtual void view_cut_selection(u64 view_id) override;
    virtual void view_paste(u64 view_id, Web::Clipboard::PasteMode) override;
    virtual void view_select_all(u64 view_id) override;
    virtual void view_get_source(u64 view_id) override;
    virtual void view_traverse_the_history_by_delta(u64 view_id, i32 delta) override;
    virtual void view_translate_to(u64 view_id, String target_language) override;
    virtual void view_show_original(u64 view_id) override;
    virtual void view_download_translation_languages(u64 view_id, String target_language) override;
    virtual void view_prefer_higher_quality_translation(u64 view_id, bool prefers_higher_quality) override;
    virtual void view_download_url(u64 view_id, URL::URL url, bool prompt_for_path, Optional<String> image_mime_type) override;
    virtual void view_inspect_context_menu_element(u64 view_id) override;
    virtual void view_replace_context_menu_misspelling(u64 view_id, Utf16String replacement) override;
    virtual void view_ignore_misspelling(u64 view_id, Utf16String word) override;
    virtual void view_learn_misspelling(u64 view_id, Utf16String word) override;
    virtual void view_do_context_menu_media_action(u64 view_id, WebView::ContextMenuMediaAction action) override;
    virtual void view_debug_request(u64 view_id, ByteString request, ByteString argument) override;
    virtual void view_dump_gc_graph(u64 view_id) override;
    virtual void view_zoom_in(u64 view_id) override;
    virtual void view_zoom_out(u64 view_id) override;
    virtual void view_reset_zoom(u64 view_id) override;
    virtual void view_set_display_metadata(u64 view_id, Optional<u64> display_id, double maximum_frames_per_second) override;
    virtual void view_set_scales_frames_to_fit(u64 view_id, bool scales_frames_to_fit) override;
    virtual void view_enable_picture_in_picture_controls(u64 view_id) override;
    virtual void view_update_interface_color_scheme(u64 view_id) override;
    virtual void view_find_in_page_next_match(u64 view_id) override;
    virtual void view_find_in_page_previous_match(u64 view_id) override;
    virtual void view_find_in_page_end(u64 view_id) override;
    virtual void view_report_website_issue(u64 view_id) override;
    virtual void view_request_accessibility_tree(u64 view_id) override;
    virtual void view_perform_accessibility_action(u64 view_id, i64 node_id, String action) override;
    virtual void view_clear_node_picker(u64 view_id) override;
    virtual void view_node_picker_hover(u64 view_id, Web::DevicePixelPoint position) override;
    virtual void view_node_picker_pick(u64 view_id, Web::DevicePixelPoint position) override;
    virtual void view_node_picker_preview(u64 view_id, Web::DevicePixelPoint position) override;
    virtual void view_node_picker_cancel(u64 view_id) override;
    virtual void view_alert_closed(u64 view_id) override;
    virtual void view_confirm_closed(u64 view_id, bool accepted) override;
    virtual void view_prompt_closed(u64 view_id, Optional<Utf16String> response) override;
    virtual void view_color_picker_update(u64 view_id, Optional<Color> picked_color, Web::HTML::ColorPickerUpdateState state) override;
    virtual void view_date_picker_closed(u64 view_id, Optional<Utf16String> picked_value) override;
    virtual void view_select_dropdown_closed(u64 view_id, Optional<u32> selected_item_id) override;
    virtual void view_set_marked_text_from_input_method(u64 view_id, Utf16String text) override;
    virtual void view_commit_text_from_input_method(u64 view_id, Utf16String text, i32 replacement_start, i32 replacement_length) override;
    virtual void view_toggle_page_mute_state(u64 view_id) override;
    virtual void view_take_and_save_screenshot(u64 view_id, WebView::ViewImplementation::ScreenshotType screenshot_type) override;
    virtual void view_request_close(u64 view_id) override;
    virtual void view_exit_fullscreen(u64 view_id) override;
    virtual void view_deny_fullscreen_window(u64 view_id) override;
    virtual void view_set_is_fullscreen(u64 view_id, Web::ViewportIsFullscreen is_fullscreen) override;
    virtual void view_reset_site_permissions(u64 view_id, URL::Origin origin) override;
};

}
