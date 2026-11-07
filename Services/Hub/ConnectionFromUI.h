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
#include <LibWebView/Autocomplete.h>
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

    // The reports that the UI has asked us to post, which it has no network to do itself.
    HashMap<u64, NonnullRefPtr<Requests::Request>> m_report_posts;
    u64 m_last_applied_settings_change_id { 0 };

    // The location bars of the UI each have one of these, which needs the network and the history.
    HashMap<u64, NonnullOwnPtr<WebView::Autocomplete>> m_omnibox_clients;

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
    virtual void post_report(u64 post_id, URL::URL url, Vector<HTTP::Header> headers, ByteString body) override;
    virtual void cancel_report_post(u64 post_id) override;
    virtual void open_bookmark_in_new_tab(String bookmark_id, Web::HTML::ActivateTab activate_tab) override;
    virtual void toggle_devtools_enabled() override;
    virtual void launch_devtools_client() override;
    virtual void reset_private_browsing_session() override;
    virtual void notify_compositor_gpu_presentation_unavailable() override;
    virtual void ui_info_changed(Hub::UIInfo info) override;
    virtual Messages::HubServer::ProcessSnapshotsResponse process_snapshots() override;
    virtual Messages::HubServer::TerminateProcessResponse terminate_process(u64 child_id) override;
    virtual Messages::HubServer::ViewNeedsBeforeunloadCheckResponse view_needs_beforeunload_check(u64 view_id) override;
    virtual Messages::HubServer::ViewRecentHistoryEntriesResponse view_recent_history_entries(u64 view_id, u64 limit) override;
    virtual Messages::HubServer::ViewSessionHistoryTraversalMenuItemsResponse view_session_history_traversal_menu_items(u64 view_id, i32 direction) override;
    virtual Messages::HubServer::ViewSessionHistorySnapshotResponse view_session_history_snapshot(u64 view_id) override;
    virtual Messages::HubServer::ViewRestoreSessionHistoryFromSnapshotResponse view_restore_session_history_from_snapshot(u64 view_id, WebView::SessionHistorySnapshot snapshot) override;
    virtual Messages::HubServer::ViewHistoryStepAtDeltaResponse view_history_step_at_delta(u64 view_id, i32 delta) override;
    virtual Messages::HubServer::ViewLookUpSelectedTextAtResponse view_look_up_selected_text_at(u64 view_id, Gfx::IntPoint position) override;
    virtual Messages::HubServer::ViewPageDescriptionResponse view_page_description(u64 view_id) override;
    virtual Messages::HubServer::ViewTranslationMenuResponse view_translation_menu(u64 view_id) override;
    virtual Messages::HubServer::SessionWindowOpenedResponse session_window_opened(WebView::IsPrivate is_private) override;
    virtual Messages::HubServer::SessionWindowClosingResponse session_window_closing(WebView::IsPrivate is_private, WebView::SessionStore::WindowClosing closing) override;
    virtual void session_window_activated(WebView::IsPrivate is_private, WebView::SessionWindowId id) override;
    virtual void session_window_geometry_changed(WebView::IsPrivate is_private, WebView::SessionWindowId id, WebView::SessionWindowGeometry geometry) override;
    virtual Messages::HubServer::SessionTabOpenedResponse session_tab_opened(WebView::IsPrivate is_private, WebView::SessionStore::TabOpened opened) override;
    virtual Messages::HubServer::SessionTabClosedResponse session_tab_closed(WebView::IsPrivate is_private, WebView::SessionStore::TabClosed closed) override;
    virtual void session_tab_detached(WebView::IsPrivate is_private, WebView::SessionTabId id) override;
    virtual void session_tab_moved(WebView::IsPrivate is_private, WebView::SessionStore::TabMoved moved) override;
    virtual void session_tab_order_changed(WebView::IsPrivate is_private, WebView::SessionStore::TabOrderChanged changed) override;
    virtual Messages::HubServer::SessionTabPinnedChangedResponse session_tab_pinned_changed(WebView::IsPrivate is_private, WebView::SessionTabId id, bool pinned) override;
    virtual void session_active_tab_changed(WebView::IsPrivate is_private, WebView::SessionTabId id) override;
    virtual void session_application_quitting(WebView::IsPrivate is_private) override;
    virtual void session_application_quit_aborted(WebView::IsPrivate is_private) override;
    virtual Messages::HubServer::SessionConsecutiveUncleanExitsResponse session_consecutive_unclean_exits(WebView::IsPrivate is_private) override;
    virtual Messages::HubServer::SessionHasClosedUnitsResponse session_has_closed_units(WebView::IsPrivate is_private) override;
    virtual Messages::HubServer::SessionTakeMostRecentlyClosedResponse session_take_most_recently_closed(WebView::IsPrivate is_private) override;
    virtual Messages::HubServer::SessionRecentlyClosedUnitsResponse session_recently_closed_units(WebView::IsPrivate is_private, u64 limit) override;
    virtual Messages::HubServer::SessionTakeClosedUnitResponse session_take_closed_unit(WebView::IsPrivate is_private, WebView::SessionWindowId id) override;
    virtual Messages::HubServer::SessionTakeLaunchPlanResponse session_take_launch_plan(WebView::IsPrivate is_private) override;
    virtual Messages::HubServer::SessionLoadTabHistoryResponse session_load_tab_history(WebView::IsPrivate is_private, WebView::SessionTabId id) override;
    virtual Messages::HubServer::HistoryEntryForUrlResponse history_entry_for_url(WebView::IsPrivate is_private, URL::URL url) override;
    virtual void view_set_site_permission(u64 view_id, URL::Origin origin, Web::Permissions::PermissionName name, Optional<WebView::PermissionDecision> decision) override;
    virtual void view_did_show_permission_prompt(u64 view_id, WebView::PermissionPromptRequest request) override;
    virtual void view_load_navigation(u64 view_id, Web::HTML::PreparedNavigationDescriptor navigation) override;
    virtual void view_open_navigation_in_new_tab(u64 view_id, Web::HTML::PreparedNavigationDescriptor navigation, Web::HTML::ActivateTab activate_tab) override;
    virtual void view_open_navigation_in_new_window(u64 view_id, Web::HTML::PreparedNavigationDescriptor navigation, WebView::IsPrivate is_private) override;
    virtual void view_traverse_the_history_to_step(u64 view_id, i32 step) override;
    virtual void view_set_session_tab_id(u64 view_id, Optional<WebView::SessionTabId> session_tab_id) override;
    virtual void view_file_picker_closed(u64 view_id, Vector<Web::HTML::SelectedFile> selected_files) override;
    virtual void view_save_screenshot(u64 view_id, Gfx::ShareableBitmap bitmap) override;
    virtual void view_request_selected_text(u64 view_id, u64 request_id) override;
    virtual void view_request_close_preflight(u64 view_id, u64 request_id) override;
    virtual void view_request_page_dumps(u64 view_id, u64 request_id) override;
    virtual void view_prepare_for_immediate_close(u64 view_id) override;
    virtual void view_prepare_for_close_without_prompting(u64 view_id, u64 close_id) override;
    virtual void finish_prepared_close(u64 close_id) override;
    virtual void open_microphone_system_settings() override;
    virtual void pip_did_resize(u64 view_id, Gfx::IntSize size) override;
    virtual void pip_did_close(u64 view_id, bool video_keeps_playing) override;
    virtual void notification_shown(u64 id) override;
    virtual void notification_failed(u64 id) override;
    virtual void notification_clicked(u64 id) override;
    virtual void notification_closed(u64 id, bool by_user) override;
    virtual void external_url_handler_resolved(u64 request_id, Optional<u64> handler_id, String application_name, bool is_ladybird) override;
    virtual void external_url_launched(u64 request_id, bool launched) override;
    virtual void omnibox_open(u64 client_id, WebView::IsPrivate is_private) override;
    virtual void omnibox_close(u64 client_id) override;
    virtual void omnibox_query(u64 client_id, u64 query_id, String query, u64 max_suggestions) override;
    virtual void omnibox_cancel(u64 client_id) override;
    virtual void omnibox_record_engagement(u64 client_id, WebView::OmniboxEngagement engagement) override;
    virtual void update_bookmarks(ByteString bookmarks_json) override;
    virtual void answer_bookmarks_for_all_tabs(u64 request_id, Vector<WebView::BookmarkItem::Bookmark> bookmarks) override;
    virtual void answer_bookmark_dialog(u64 request_id, Optional<Hub::BookmarkDialogResult> result) override;
    virtual void bookmark_context_menu_item_changed(Optional<String> item_id, Optional<String> target_folder_id, Optional<String> parent_folder_id) override;
    virtual void cancel_download(u64 id) override;
    virtual void pause_download(u64 id) override;
    virtual void resume_download(u64 id) override;
    virtual void show_download_in_folder(u64 id) override;
    virtual void prune_inactive_downloads(WebView::IsPrivate is_private) override;
    virtual void pause_active_downloads() override;
    virtual void cancel_unresumable_downloads() override;
    virtual void active_view_changed(Optional<u64> view_id) override;
    virtual void answer_clipboard_text(u64 request_id, Utf16String text) override;
    virtual void answer_clipboard_item(u64 request_id, Web::Clipboard::SystemClipboardItem item) override;
    virtual void answer_download_path(u64 request_id, Optional<ByteString> path) override;
    virtual void update_settings(u64 change_id, ByteString changes_json) override;
    virtual void activate_action(u64 handle, Optional<bool> checked) override;
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
    virtual void view_load_url_from_user_input(u64 view_id, URL::URL url) override;
    virtual void view_load_from_user_input(u64 view_id, ByteString input, Optional<URL::URL> fallback_url) override;
    virtual void view_load_html(u64 view_id, ByteString html) override;
    virtual void view_load_navigation_error_page(u64 view_id, ByteString html) override;
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
