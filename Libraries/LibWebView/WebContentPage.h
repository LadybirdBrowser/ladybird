/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashTable.h>
#include <AK/Noncopyable.h>
#include <AK/Optional.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/String.h>
#include <AK/WeakPtr.h>
#include <LibCompositing/InputEvent.h>
#include <LibCompositing/PageId.h>
#include <LibCompositing/Types.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibGfx/SharedImage.h>
#include <LibWeb/HTML/CrossProcessId.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/StorageAPI/StorageEndpoint.h>
#include <LibWebView/Export.h>
#include <LibWebView/Forward.h>
#include <WebContent/WebContentClientEndpoint.h>
#include <WebContent/WebContentServerEndpoint.h>
#include <WebContent/WebContentTestClientEndpoint.h>

namespace WebView {

class WEBVIEW_API WebContentPage final
    : public RefCounted<WebContentPage>
    , public WebContentClientPageStub
    , public WebContentTestClientPageStub
    , public WebContentServerPageProxy<WebContentPage> {
    AK_MAKE_NONCOPYABLE(WebContentPage);
    AK_MAKE_NONMOVABLE(WebContentPage);
    AK_ALLOC_WITH_KMALLOC;

    friend class WebContentClient;
    friend class WebContentTestClient;

public:
    WebContentPage(WebContentClient&, Compositing::PageId, CanonicalTraversable&);
    virtual ~WebContentPage() override;

    WebContentClient& client() const;
    Compositing::PageId id() const { return m_id; }
    WebContentClient* routed_connection() const { return m_client.ptr(); }
    Compositing::PageId routed_page_id() const { return m_id; }

    CanonicalTraversable& traversable() const;
    ViewImplementation& view() const;
    bool displays_tab() const;
    Optional<CanonicalNavigable&> hosted_navigable(Web::HTML::CrossProcessId) const;
    // The process and page hosting the document of a navigable that a page represents. A page represents every
    // navigable of its tab whose document it does not host, so those are the ones it can ask to navigate or post to.
    RefPtr<WebContentPage> endpoint_hosting_navigable_represented_by(Web::HTML::CrossProcessId navigable_id) const;
    RefPtr<WebContentPage> page_hosting_container_of(Web::HTML::CrossProcessId navigable_id) const;
    RefPtr<WebContentPage> page_hosting_navigable(Web::HTML::CrossProcessId navigable_id) const;

    // False once the page can no longer host work: the page is unregistered or the process is gone. A page
    // awaiting a detached close remains open; it still coordinates its own close.
    bool is_open() const { return m_is_open; }
    void close();

    bool needs_beforeunload_check() const { return m_needs_beforeunload_check; }
    bool detached_close_pending() const { return m_detached_close_pending; }
    void set_detached_close_pending(bool pending) { m_detached_close_pending = pending; }
    void set_needs_beforeunload_check(bool needs_beforeunload_check) { m_needs_beforeunload_check = needs_beforeunload_check; }
    void clear_history_recorded_url_for_current_load() { m_history_recorded_url_for_current_load.clear(); }

    void begin_top_level_load(Optional<Utf16String> navigation_id, URL::URL const&);

    void request_close();
    void discard();

    Compositing::CompositorContextId compositor_context_id();
    bool send_async_scroll_to_compositor(Gfx::FloatPoint position, Gfx::FloatPoint delta_in_device_pixels, Compositing::WheelDeltaPrecision, Compositing::ScrollGesturePhase, u32 modifiers);
    bool handle_key_event_in_compositor(Compositing::KeyEvent const&);
    void dispatch_key_event_to_web_content(Compositing::KeyEvent const&);
    bool handle_pinch_event_in_compositor(Compositing::PinchEvent const&);
    Compositing::MouseEventHandlingResult handle_mouse_event_in_compositor(Compositing::MouseEvent const&);
    void dispatch_mouse_event_to_web_content(Compositing::MouseEvent const&);
    void did_present_bitmap(Gfx::IntRect content_rect, Gfx::IntRect damage_rect, i32 bitmap_id);
    void did_present_backing_stores(Vector<i32> bitmap_ids, Vector<Gfx::SharedImage> backing_stores);
    void release_presented_bitmap(i32 bitmap_id);
    void fail_renderer_owned_downloads();

private:
    Optional<CanonicalNavigable&> population_worker_navigable(Web::HTML::CrossProcessId navigable_id) const;
    bool continue_navigation_population_in_selected_process(Web::HTML::CrossProcessId navigable_id, Utf16String navigation_id);
    StorageJar* storage_jar(Web::StorageAPI::StorageEndpointType) const;
    struct ViewPosition {
        ViewImplementation& view;
        Gfx::IntPoint position;
    };
    Optional<ViewPosition> view_position(Web::HTML::CrossProcessId local_root_id, Gfx::IntPoint) const;
    void did_open_dialog(Web::Page::PendingDialog, Utf16String const& message);
    void maybe_record_history_visit_for_current_load(URL::URL const&, Optional<String> title, StringView reason);

    virtual void did_request_navigation_of_navigable(Web::HTML::CrossProcessId navigable_id, Web::HTML::PreparedNavigationDescriptor navigation) override;
    virtual void did_post_message_to_navigable(Web::HTML::CrossProcessId navigable_id, Web::HTML::PostedMessageDescriptor message) override;
    virtual void did_request_focusing_steps_for_navigable(Web::HTML::CrossProcessId navigable_id, Web::HTML::FocusTrigger focus_trigger) override;
    virtual void did_request_window_focus_of_navigable(Web::HTML::CrossProcessId navigable_id) override;
    virtual void did_request_set_opener_of_navigable(Web::HTML::CrossProcessId navigable_id, Web::HTML::CrossProcessId opener_navigable_id) override;
    virtual void did_completely_finish_loading(Web::HTML::CrossProcessId navigable_id) override;
    virtual void did_create_child_frame(Web::HTML::CrossProcessId parent_frame_id, Web::HTML::CrossProcessId frame_id, Web::HTML::ReplicatedNavigableState replicated_state) override;
    virtual void did_set_browser_zoom(double factor) override;
    virtual void did_find_in_page(size_t current_match_index, Optional<size_t> total_match_count) override;
    virtual void did_request_refresh() override;
    virtual void did_request_cursor_change(Gfx::Cursor cursor) override;
    virtual void did_update_editing_history_state(bool can_undo, bool can_redo) override;
    virtual void did_request_tooltip_override(Gfx::IntPoint position, ByteString title) override;
    virtual void did_stop_tooltip_override() override;
    virtual void did_enter_tooltip_area(ByteString title) override;
    virtual void did_leave_tooltip_area() override;
    virtual void did_hover_link(URL::URL url) override;
    virtual void did_unhover_link() override;
    virtual void did_click_link(URL::URL url, ByteString target, unsigned modifiers) override;
    virtual void did_middle_click_link(URL::URL url, ByteString, unsigned) override;
    virtual void did_request_external_url(URL::URL url, URL::Origin initiator_origin, bool has_transient_activation) override;
    virtual void did_inspect_storage(u64 request_id, String storage_items) override;
    virtual void did_inspect_grid_layouts(String grid_layouts) override;
    virtual void did_inspect_current_grid(String grid_layout) override;
    virtual void did_inspect_current_flexbox(String flexbox_layout) override;
    virtual void did_inspect_indexed_database(u64 request_id, String result) override;
    virtual void did_inspect_accessibility_tree(String accessibility_tree) override;
    virtual void did_get_hovered_node_id(Compositing::UniqueNodeID node_id) override;
    virtual void did_get_node_id_at_position(u64 request_id, Compositing::UniqueNodeID node_id) override;
    virtual void did_list_style_sheets(Vector<Web::CSS::StyleSheetIdentifier> stylesheets) override;
    virtual void did_get_style_sheet_source(Web::CSS::StyleSheetIdentifier identifier, URL::URL base_url, Utf16String source) override;
    virtual void did_list_devtools_sources(u64 request_id, Vector<Web::HTML::ScriptRegistry::Description> sources) override;
    virtual void did_get_devtools_source(Web::HTML::ScriptRegistry::Identifier source_id, Optional<Web::HTML::ScriptRegistry::Content> source) override;
    virtual void did_add_devtools_source(Web::HTML::ScriptRegistry::Description source) override;
    virtual void did_pause_debugger(DebuggerPause pause) override;
    virtual void did_resume_debugger() override;
    virtual void did_complete_debugger_breakpoint_operation(u64 request_id, Optional<String> error) override;
    virtual void did_take_screenshot(Gfx::ShareableBitmap screenshot) override;
    virtual void did_get_internal_page_info(WebView::PageInfoType type, Optional<Core::AnonymousBuffer> info) override;
    virtual void did_get_selected_text(u64 request_id, ByteString selection) override;
    virtual void did_get_selected_text_for_lookup(u64 request_id, Optional<DictionaryLookup> lookup) override;
    virtual void did_select_word_for_dictionary_lookup(u64 request_id, bool selected) override;
    virtual void did_cut_selected_text(u64 request_id, ByteString selection) override;
    virtual void did_execute_js_console_input(JsonValue result) override;
    virtual void did_output_js_console_message(ConsoleOutput console_output) override;
    virtual void did_start_network_request(u64 request_id, URL::URL url, ByteString method, Vector<HTTP::Header> request_headers, ByteBuffer request_body, Optional<String> initiator_type, String referrer_policy, bool is_navigation_request, Web::Fetch::Infrastructure::Request::Priority priority) override;
    virtual void did_receive_network_response_body(u64 request_id, ByteBuffer data) override;
    virtual void did_finish_network_request(u64 request_id, u64 body_size, Requests::RequestTimingInfo timing_info, Optional<Requests::NetworkError> network_error) override;
    virtual void did_request_set_prompt_text(Utf16String message) override;
    virtual void did_request_accept_dialog() override;
    virtual void did_request_dismiss_dialog() override;
    virtual void did_request_document_cookie_version_index(i64 document_id, String domain) override;
    Messages::WebContentClient::DidRequestStorageItemResponse did_request_storage_item(Web::StorageAPI::StorageEndpointType storage_endpoint, String storage_key, Utf16String bottle_key);
    Messages::WebContentClient::DidSetStorageItemResponse did_set_storage_item(Web::StorageAPI::StorageEndpointType storage_endpoint, String storage_key, Utf16String bottle_key, Utf16String value);
    virtual void did_remove_storage_item(Web::StorageAPI::StorageEndpointType storage_endpoint, String storage_key, Utf16String bottle_key) override;
    Messages::WebContentClient::DidRequestStorageKeysResponse did_request_storage_keys(Web::StorageAPI::StorageEndpointType storage_endpoint, String storage_key);
    virtual void did_clear_storage(Web::StorageAPI::StorageEndpointType storage_endpoint, String storage_key) override;
    virtual void did_request_activate_tab() override;
    virtual void did_change_needs_beforeunload_check(bool needs_beforeunload_check) override;
    virtual void did_consume_user_activation(Web::HTML::UserActivationConsumption consumption) override;
    virtual void webdriver_user_prompt_handling_complete(u64 request_id, Web::WebDriver::Response response) override;
    virtual void webdriver_did_set_current_browsing_context(u64 command_id, Web::HTML::CrossProcessId navigable_id) override;
    virtual void webdriver_command_complete(u64 command_id, Web::WebDriver::Response response) override;
    virtual void did_update_resource_count(i32 count_waiting) override;
    virtual void did_request_restore_window() override;
    virtual void did_request_reposition_window(Gfx::IntPoint position, u64 completion_id) override;
    virtual void did_request_resize_window(Gfx::IntSize size, u64 completion_id) override;
    virtual void did_request_maximize_window(u64 completion_id) override;
    virtual void did_request_minimize_window() override;
    virtual void did_request_fullscreen_window() override;
    virtual void did_request_exit_fullscreen() override;
    virtual void did_request_file(ByteString path, i32 request_id) override;
    virtual void did_request_color_picker(Color current_color) override;
    virtual void did_request_geolocation_position(u64 request_id) override;
    virtual void did_cancel_geolocation_position_request(u64 request_id) override;
    virtual void did_start_geolocation_position_watch(u64 request_id) override;
    virtual void did_stop_geolocation_position_watch(u64 request_id) override;
    virtual void did_request_file_picker(Web::HTML::FileFilter accepted_file_types, Web::HTML::AllowMultipleFiles allow_multiple_files) override;
    virtual void did_finish_handling_input_event(u64 event_id, Web::EventResult event_result) override;
    virtual void did_update_input_method_state(Optional<Compositing::DevicePixelRect> caret_rect, bool is_enabled, i32 cursor_position, i32 anchor_position, Utf16String text_before_cursor, Utf16String text_after_cursor) override;
    virtual void did_change_theme_color(Gfx::Color color) override;
    virtual void did_change_background_color(Gfx::Color color) override;
    virtual void did_insert_clipboard_item(Web::Clipboard::SystemClipboardItem item, String) override;
    virtual void did_change_audio_play_state(Web::HTML::AudioPlayState play_state) override;
    virtual void did_change_screen_wake_lock_state(Web::ScreenWakeLockState wake_lock_state) override;
    virtual void did_update_session_history_entry_navigation_api_state(Web::HTML::CrossProcessId navigable_id, Web::HTML::SessionHistoryEntryIdentity entry_identity, Web::HTML::StorageSerializationRecord navigation_api_state) override;
    virtual void did_update_session_history_entry_document_state_navigable_target_name(Web::HTML::CrossProcessId navigable_id, Web::HTML::SessionHistoryEntryIdentity entry_identity, Utf16String navigable_target_name) override;
    virtual void did_set_session_history_entry_document_state_reload_pending(Web::HTML::CrossProcessId navigable_id, Utf16String navigation_api_key, bool reload_pending) override;
    virtual void did_change_focused_navigable(Web::HTML::CrossProcessId navigable_id) override;
    virtual void did_request_key_event_for_testing(Compositing::KeyEvent event) override;
    virtual void request_history_operation(Web::HTML::CrossProcessId operation_id, Web::HistoryOperationParameters parameters) override;
    virtual void history_operation_ready(Web::HTML::CrossProcessId operation_id, Web::HistoryOperationReadyResult result) override;
    virtual void history_step_unload_cancelation_result(Web::HTML::CrossProcessId operation_id, Web::HTML::HistoryStepResult result, Web::HTML::UnloadPromptShown unload_prompt_shown) override;
    virtual void beforeunload_check_result(Web::HTML::CrossProcessId operation_id, Web::HTML::HistoryStepResult result, Web::HTML::UnloadPromptShown unload_prompt_shown) override;
    virtual void changing_navigable_history_job_ready(Web::HTML::CrossProcessId operation_id, Web::HTML::CrossProcessId navigable_id, Web::HTML::ChangingNavigableHistoryStepJobDisposition disposition, Web::HTML::UnloadDisplayedDocument unload_displayed_document) override;
    virtual void changing_navigable_unload_preparation_complete(Web::HTML::CrossProcessId operation_id, Web::HTML::CrossProcessId navigable_id) override;
    virtual void descendant_unload_task_complete(Web::HTML::CrossProcessId unload_id, Web::HTML::CrossProcessId navigable_id) override;
    virtual void request_navigable_document_abort(Web::HTML::CrossProcessId navigable_id) override;
    virtual void request_navigable_document_unfullscreen(Web::HTML::CrossProcessId navigable_id) override;
    virtual void request_navigable_container_fullscreen(Web::HTML::CrossProcessId navigable_id, Web::HTML::CrossProcessId requesting_navigable_id, Web::Fullscreen::RequestType request_type) override;
    virtual void navigable_container_fullscreen_complete(Web::HTML::CrossProcessId requesting_navigable_id) override;
    virtual void request_navigable_container_unfullscreen(Web::HTML::CrossProcessId navigable_id) override;
    virtual void navigable_container_unfullscreen_complete(Web::HTML::CrossProcessId navigable_id) override;
    virtual void request_fully_exit_fullscreen() override;
    virtual void request_child_navigable_unload(Web::HTML::CrossProcessId navigable_id) override;
    virtual void changing_navigable_continuation_applied(Web::HTML::CrossProcessId operation_id, Web::HTML::CrossProcessId navigable_id, Optional<Web::HTML::ReplicatedNavigableState> activated_navigable_state, Optional<Web::HTML::SessionHistoryEntryPersistedState> previous_entry_persisted_state) override;
    virtual void nonchanging_navigable_history_state_updated(Web::HTML::CrossProcessId operation_id, Web::HTML::CrossProcessId navigable_id) override;
    virtual void did_request_close_of_traversable(Web::HTML::CrossProcessId navigable_id, Web::HTML::CrossProcessId source_navigable_id) override;
    virtual void did_inspect_dom_tree(String dom_tree) override;
    virtual void did_inspect_dom_node(DOMNodeProperties properties) override;
    virtual void did_finish_editing_dom_node(Optional<Compositing::UniqueNodeID> node_id) override;
    virtual void did_mutate_dom(Mutation mutation) override;
    virtual void did_get_dom_node_html(String html) override;
    virtual void did_resolve_dom_node_url(u64 request_id, String resolved_url) override;
    virtual void did_receive_network_response_headers(u64 request_id, u32 status_code, Optional<String> reason_phrase, Vector<HTTP::Header> response_headers, Requests::CameFromCache came_from_cache) override;
    virtual void did_change_storage_item(Web::StorageAPI::StorageEndpointType storage_endpoint, String url, Optional<Utf16String> key, Optional<Utf16String> old_value, Optional<Utf16String> new_value) override;
    virtual void did_update_indexed_database(String update) override;
    virtual void did_request_clipboard_entries(u64 request_id) override;
    virtual void did_request_set_system_focus(bool has_system_focus) override;
    virtual void did_request_set_system_visibility_state(Web::HTML::VisibilityState visibility_state) override;
    virtual void did_request_navigation_start(Web::HTML::CrossProcessId navigable_id, Web::NavigationTarget target, URL::URL url, Utf16String navigation_id, Optional<Web::HTML::NavigationStartRequest> start_request) override;
    virtual void did_complete_navigation_unload_check(Web::HTML::CrossProcessId navigable_id, Utf16String navigation_id) override;
    virtual void did_request_navigation_population(Web::HTML::CrossProcessId navigable_id, Web::NavigationTarget target, Web::HTML::NavigationPopulationRequest request) override;
    virtual void did_finish_navigation_params_creation(Web::HTML::CrossProcessId navigable_id, Utf16String navigation_id, Optional<Web::HTML::NavigationPopulationResult> result) override;
    virtual void did_finish_history_navigation_params_creation(Web::HTML::CrossProcessId operation_id, Web::HTML::HistoryNavigationPopulation population) override;
    virtual void did_fail_navigation_population(Web::HTML::CrossProcessId navigable_id, Utf16String navigation_id) override;
    virtual void did_change_replicated_navigable_state(Web::HTML::CrossProcessId navigable_id, Web::HTML::ReplicatedNavigableState state) override;
    virtual void did_change_navigable_container_state(Web::HTML::CrossProcessId navigable_id, Web::HTML::ReplicatedContainerState state) override;
    virtual void did_update_child_frame_viewport(Web::HTML::CrossProcessId frame_id, Compositing::DevicePixelRect viewport_rect, Compositing::DevicePixelRect viewport_intersection, double device_pixel_ratio) override;
    virtual void did_forward_mouse_event_to_child_frame(Web::HTML::CrossProcessId frame_id, Compositing::MouseEvent) override;
    virtual void did_destroy_child_frame(Web::HTML::CrossProcessId frame_id) override;
    Messages::WebContentClient::DidStartDownloadWithoutRequestResponse did_start_download_without_request(URL::URL url, ByteString suggested_filename, Optional<u64> total_size);
    Messages::WebContentClient::DidStartDownloadResponse did_start_download(Web::HTML::CrossProcessId navigable_id, Optional<Utf16String> navigation_id, URL::URL url, ByteString suggested_filename, Optional<u64> total_size, int request_server_client_id, u64 request_server_request_id, ByteBuffer initial_data);
    virtual void did_receive_download_data(u64 download_id, ByteBuffer data) override;
    virtual void did_finish_download(u64 download_id) override;
    virtual void did_fail_download(u64 download_id, String error) override;
    virtual void did_finish_loading(Web::HTML::CrossProcessId navigable_id, Optional<Utf16String> navigation_id) override;
    virtual void did_change_title(Utf16String title) override;
    virtual void did_request_context_menu(Web::HTML::CrossProcessId local_root_id, Gfx::IntPoint content_position, Web::ContextMenuForInputEventsTarget for_input_events_target) override;
    virtual void did_request_link_context_menu(Web::HTML::CrossProcessId local_root_id, Gfx::IntPoint content_position, URL::URL url, ByteString, unsigned) override;
    virtual void did_request_image_context_menu(Web::HTML::CrossProcessId local_root_id, Gfx::IntPoint content_position, URL::URL url, ByteString, unsigned, Optional<Gfx::ShareableBitmap> bitmap) override;
    virtual void did_request_media_context_menu(Web::HTML::CrossProcessId local_root_id, Gfx::IntPoint content_position, ByteString, unsigned, Web::Page::MediaContextMenu menu) override;
    virtual void did_get_source(URL::URL url, URL::URL base_url, Utf16String source) override;
    virtual void did_get_debugger_environments(u64 request_id, Optional<String> error, Vector<DebuggerEnvironment> environments) override;
    virtual void did_evaluate_javascript_in_debugger_frame(u64 request_id, Optional<String> error, DebuggerEvaluationResult result) override;
    virtual void did_get_debugger_object_properties(u64 request_id, Optional<String> error, DebuggerObjectProperties properties) override;
    virtual void did_get_debugger_source_positions(u64 request_id, Vector<DebuggerSourcePosition> positions) override;
    virtual void did_request_alert(Utf16String message) override;
    virtual void did_request_confirm(Utf16String message) override;
    virtual void did_request_prompt(Utf16String message, Utf16String default_) override;
    virtual void did_change_favicon(Gfx::ShareableBitmap favicon) override;
    virtual void did_request_delete_all_cookies(u64 request_id, URL::URL url) override;
    Messages::WebContentClient::DidRequestNewWebViewResponse did_request_new_web_view(Web::HTML::ActivateTab activate_tab, Web::HTML::WebViewHints hints, Optional<Web::HTML::CrossProcessId> opener_navigable_id, Optional<URL::URL> opener_base_url, Utf16String target_name);
    void did_close_browsing_context();
    virtual void did_request_select_dropdown(Web::HTML::CrossProcessId local_root_id, Gfx::IntPoint content_position, i32 minimum_width, Vector<Web::HTML::SelectItem> items) override;
    virtual void did_request_primary_paste() override;
    virtual void did_update_primary_selection(String text) override;
    virtual void did_update_session_history_entry_scroll_restoration_mode(Web::HTML::CrossProcessId navigable_id, Web::HTML::SessionHistoryEntryIdentity entry_identity, Web::HTML::ScrollRestorationMode scroll_restoration_mode) override;
    virtual void did_request_webdriver_mouse_event(u64 request_id, Web::HTML::CrossProcessId local_root_id, Compositing::MouseEvent event) override;
    virtual void request_unload_check(Web::HTML::CrossProcessId navigable_id, Web::HTML::CrossProcessId check_id) override;
    Messages::WebContentClient::StartWorkerAgentResponse start_worker_agent(Web::HTML::WorkerAgentStartRequest request);
    Messages::WebContentClient::DidRequestStorageUsageResponse did_request_storage_usage(String storage_key);
    virtual void did_post_broadcast_channel_message(Web::HTML::BroadcastChannelMessage message) override;
    virtual void close_worker_agent(Web::HTML::WorkerAgentId agent_id, Web::HTML::WorkerAgentOwnerToken owner_token) override;
    Messages::WebContentClient::DidRequestCookieResponse did_request_cookie(URL::URL, HTTP::Cookie::Source);

    // Test-only handlers, reached over the separate test transport (see WebContentTestClient).
    virtual void did_finish_test(String text) override;
    virtual void did_set_test_timeout(double milliseconds) override;
    virtual void did_receive_reference_test_metadata(JsonValue) override;
    virtual void did_simulate_worker_request_server_connection_loss() override;
    Messages::WebContentTestClient::DidRequestUiProcessSessionHistoryForTestingResponse did_request_ui_process_session_history_for_testing();
    Messages::WebContentTestClient::DidRequestSiteIsolationProcessTreeForTestingResponse did_request_site_isolation_process_tree_for_testing();
    virtual void did_request_crash_of_remote_frame_processes_for_testing() override;
    virtual void did_reset_session_history_for_testing(Web::HTML::SessionHistoryEntryDescriptor) override;
    Messages::WebContentTestClient::DidRequestCaptureSessionHistorySnapshotForTestingResponse did_request_capture_session_history_snapshot_for_testing();
    Messages::WebContentTestClient::DidRequestRestoreSessionHistorySnapshotForTestingResponse did_request_restore_session_history_snapshot_for_testing();
    Messages::WebContentTestClient::DidRequestRegisterSessionStoreTabForTestingResponse did_request_register_session_store_tab_for_testing();
    Messages::WebContentTestClient::DidRequestSessionStoreTabStateForTestingResponse did_request_session_store_tab_state_for_testing();

    WeakPtr<WebContentClient> m_client;
    Compositing::PageId m_id;
    WeakPtr<CanonicalTraversable> m_traversable;
    bool m_is_open { true };
    bool m_needs_beforeunload_check { true };
    bool m_detached_close_pending { false };
    Optional<String> m_history_recorded_url_for_current_load;
    HashTable<u64> m_renderer_owned_downloads;
};

}
