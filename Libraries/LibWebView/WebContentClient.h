/*
 * Copyright (c) 2020-2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/NonnullRawPtr.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/SourceLocation.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/WeakPtr.h>
#include <LibCore/Forward.h>
#include <LibGfx/Point.h>
#include <LibGfx/SharedImage.h>
#include <LibHTTP/Header.h>
#include <LibIPC/ConnectionToServer.h>
#include <LibIPC/Transport.h>
#include <LibRequests/CameFromCache.h>
#include <LibRequests/NetworkError.h>
#include <LibRequests/RequestTimingInfo.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Bindings/Navigation.h>
#include <LibWeb/CSS/StyleSheetIdentifier.h>
#include <LibWeb/Compositor/Types.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/ActivateTab.h>
#include <LibWeb/HTML/ApplyHistoryStep.h>
#include <LibWeb/HTML/CrossProcessId.h>
#include <LibWeb/HTML/FileFilter.h>
#include <LibWeb/HTML/HistoryHandlingBehavior.h>
#include <LibWeb/HTML/HistoryOperation.h>
#include <LibWeb/HTML/ReplicatedNavigableState.h>
#include <LibWeb/HTML/SameDocumentNavigationEntry.h>
#include <LibWeb/HTML/Scripting/ScriptRegistry.h>
#include <LibWeb/HTML/SelectItem.h>
#include <LibWeb/HTML/SessionHistoryEntry.h>
#include <LibWeb/HTML/UserNavigationInvolvement.h>
#include <LibWeb/HTML/VisibilityState.h>
#include <LibWeb/HTML/WebViewHints.h>
#include <LibWeb/HTML/WorkerAgentTypes.h>
#include <LibWeb/Page/EventResult.h>
#include <LibWeb/Page/PageId.h>
#include <LibWeb/Page/ScreenWakeLockHandle.h>
#include <LibWeb/Page/ViewportIsFullscreen.h>
#include <LibWeb/StorageAPI/StorageEndpoint.h>
#include <LibWebView/BlobURLStore.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/Debugger.h>
#include <LibWebView/Forward.h>
#include <WebContent/WebContentClientEndpoint.h>
#include <WebContent/WebContentServerEndpoint.h>

namespace WebView {

class ViewImplementation;

class WEBVIEW_API WebContentClient final
    : public IPC::ConnectionToServer<WebContentClientEndpoint, WebContentServerEndpoint>
    , public WebContentClientEndpoint {
    C_OBJECT_ABSTRACT(WebContentClient);

    friend class WebContentTestClient;

public:
    using InitTransport = Messages::WebContentServer::InitTransport;

    template<CallableAs<IterationDecision, WebContentClient&> Callback>
    static void for_each_client(Callback callback);

    static size_t client_count() { return clients().size(); }
    static Optional<WebContentClient&> client_for_compositor_context_id(Web::Compositor::CompositorContextId);

    virtual Messages::WebContentClient::OpenSystemFontResponse open_system_font(u64 generation, u64 face_id) override;
    virtual Messages::WebContentClient::MatchSystemFontResponse match_system_font(String family, u16 weight, u16 width, u8 slope) override;
    virtual Messages::WebContentClient::MatchLocalFontResponse match_local_font(String name) override;
    virtual Messages::WebContentClient::MatchSystemFontForCodePointResponse match_system_font_for_code_point(u32 code_point, u16 weight, u16 width, u8 slope, bool prefer_color_emoji) override;
    virtual Messages::WebContentClient::ResolveGenericFontResponse resolve_generic_font(String family, u16 weight, u8 slope) override;
    virtual Messages::WebContentClient::DidAddBlobUrlEntryResponse did_add_blob_url_entry(Utf16String url, Web::FileAPI::SerializedBlobURLEntry entry) override;
    virtual void did_remove_blob_url_entries(Vector<Utf16String> urls, URL::Origin origin) override;
    virtual void did_retain_blob_url_token(Web::HTML::CrossProcessId navigable_id, URL::BlobURLEntry::Token token) override;
    virtual Messages::WebContentClient::DidRequestBlobUrlEntryResponse did_request_blob_url_entry(Utf16String url, Optional<URL::BlobURLEntry::Token> token) override;

    WebContentClient(NonnullOwnPtr<IPC::Transport>, IsPrivate, Web::PageId initial_page_id, Web::HTML::CrossProcessId root_navigable_id);
    ~WebContentClient();

    IsPrivate is_private() const { return m_is_private; }
    BrowsingSession& session() const { return *m_session; }
    void remove_blob_url_entries();

    void connect_test_endpoint(NonnullOwnPtr<IPC::Transport>);
    // Null outside test mode: the test endpoint is only connected when the UI process runs tests.
    WebContentTestClient* test_connection() { return m_test_connection; }

    void assign_view(Badge<Application>, ViewImplementation&);
    void set_initial_top_level_history_entry(Badge<Application>, Web::HTML::SessionHistoryEntryDescriptor entry) { m_initial_top_level_history_entry = move(entry); }
    void register_view(Web::PageId page_id, ViewImplementation&);
    void unregister_view(Web::PageId page_id);

    void set_compositor_connection_id(Badge<Application>, i32);
    Optional<i32> compositor_connection_id(Badge<Application>) const { return m_compositor_connection_id; }

    void prepare_for_detached_close(Web::PageId page_id);
    void request_close(Web::PageId page_id);

    void web_ui_disconnected(Badge<WebUI>);
    void register_embedded_page(Web::PageId page_id, CanonicalTraversable&);
    void unregister_embedded_page(Web::PageId page_id);
    void keep_view_page_for_displaced_document(Web::PageId page_id, CanonicalTraversable&);
    Optional<Web::PageId> page_id_for_traversable(CanonicalTraversable const&) const;
    bool is_view_page(Web::PageId page_id) const { return m_views.contains(page_id); }
    bool page_needs_beforeunload_check(Web::PageId page_id) const { return m_needs_beforeunload_check_by_page.get(page_id).value_or(true); }

    CanonicalTraversable* traversable_for_page(Web::PageId page_id);
    // False once the page can no longer host work: the page is unregistered or the process is gone. A page
    // awaiting a detached close remains open; it still coordinates its own close.
    bool is_page_open(Web::PageId page_id) const;
    // True for every page ID the UI process has handed to this connection, closed pages included, since a
    // message the connection sent while it had the page can arrive after the page is gone.
    virtual bool may_act_for_page(Web::PageId page_id) const override;
    Optional<CanonicalNavigable&> hosted_navigable(Web::HTML::CrossProcessId navigable_id);
    Optional<CanonicalNavigable&> hosted_navigable_for_page(Web::PageId page_id, Web::HTML::CrossProcessId navigable_id);
    Optional<CanonicalNavigable&> population_worker_navigable_for_page(Web::PageId page_id, Web::HTML::CrossProcessId navigable_id);

    void begin_top_level_load(ViewImplementation&, Web::PageId page_id, Optional<Utf16String> navigation_id, URL::URL const& url);

    Optional<CanonicalNavigable&> child_frame(Web::PageId page_id, Web::HTML::CrossProcessId frame_id);

    Optional<u64> exclusive_performance_owner() const;

    bool has_views() const { return !m_views.is_empty(); }

    void notify_all_views_of_crash();
    ErrorOr<void> reconnect_to_compositor_process(Badge<Application>);
    ErrorOr<void> recreate_compositor_contexts(Badge<Application>);
    void replay_compositor_view_state_after_reconnect(Badge<Application>);
    void notify_compositor_process_reconnected(Badge<Application>);
    Web::Compositor::CompositorContextId compositor_context_id_for_page(Web::PageId page_id);
    Optional<Web::PageId> page_id_for_compositor_context_id(Web::Compositor::CompositorContextId) const;
    bool send_async_scroll_to_compositor(Web::PageId page_id, Gfx::FloatPoint position, Gfx::FloatPoint delta_in_device_pixels, Web::WheelDeltaPrecision, Web::ScrollGesturePhase);
    bool handle_mouse_event_in_compositor(Web::PageId page_id, Web::MouseEvent const&);
    bool handle_mouse_event_in_compositor(Web::PageId page_id, CanonicalNavigable const& root, Optional<Web::Compositor::CompositorContextId>, Web::MouseEvent const&);
    bool handle_key_event_in_compositor(Web::PageId page_id, Web::KeyEvent const&);
    void dispatch_key_event_to_web_content(Web::PageId page_id, Web::KeyEvent const&);
    bool handle_pinch_event_in_compositor(Web::PageId page_id, Web::PinchEvent const&);
    void dispatch_mouse_event_to_web_content(Web::PageId page_id, Web::MouseEvent const&);
    void dispatch_mouse_event_to_web_content(Web::PageId page_id, CanonicalNavigable const& root, Optional<Web::Compositor::CompositorContextId>, Web::MouseEvent const&);
    void notify_presented_bitmap_ready_to_paint(Web::PageId page_id, i32 bitmap_id);
    void did_present_backing_stores(Web::PageId page_id, Vector<i32> bitmap_ids, Vector<Gfx::SharedImage> backing_stores);
    void did_present_bitmap(Web::PageId page_id, Gfx::IntRect content_rect, Gfx::IntRect damage_rect, i32 bitmap_id);
    void close_if_unused(Badge<CanonicalNavigable>) { close_server_if_unused(); }

    pid_t pid() const { return m_process_handle.pid; }
    void set_pid(pid_t pid) { m_process_handle.pid = pid; }

private:
    friend class SiteIsolationManager;

    void maybe_record_history_visit_for_current_load(Web::PageId page_id, URL::URL const&, Optional<String> title, StringView reason);
    void close_server_if_unused();
    bool forget_compositor_context(Web::Compositor::CompositorContextId);
    void destroy_all_compositor_contexts();
    StorageJar* storage_jar_for_page(Web::PageId page_id, Web::StorageAPI::StorageEndpointType);
    void cancel_navigation_transactions();
    bool continue_navigation_population_in_selected_process(Web::PageId page_id, Web::HTML::CrossProcessId navigable_id, Utf16String navigation_id);

    virtual void did_misbehave(StringView message_name, StringView reason) override;

    virtual void die() override;

    // Test-only handlers, reached over the separate test transport (see WebContentTestClient).
    void did_finish_test(Web::PageId page_id, String text);
    void did_set_test_timeout(Web::PageId page_id, double milliseconds);
    void did_receive_reference_test_metadata(Web::PageId page_id, JsonValue);
    void did_expire_cookies_with_time_offset(AK::Duration);
    void did_simulate_worker_request_server_connection_loss(Web::PageId page_id);
    String did_request_ui_process_session_history_for_testing(Web::PageId page_id);
    String did_request_site_isolation_process_tree_for_testing(Web::PageId page_id);
    void did_request_crash_of_remote_frame_processes_for_testing(Web::PageId page_id);
    void did_reset_session_history_for_testing(Web::PageId page_id, Web::HTML::SessionHistoryEntryDescriptor);
    bool did_request_capture_session_history_snapshot_for_testing(Web::PageId page_id);
    bool did_request_restore_session_history_snapshot_for_testing(Web::PageId page_id);
    bool did_request_register_session_store_tab_for_testing(Web::PageId page_id);
    String did_request_session_store_tab_state_for_testing(Web::PageId page_id);

    virtual Messages::WebContentClient::AllocateCompositorContextIdResponse allocate_compositor_context_id(Web::PageId page_id, Web::Compositor::PagePresentationRegistration) override;
    virtual void did_destroy_compositor_context(Web::Compositor::CompositorContextId) override;
    virtual void did_request_navigation_start(Web::PageId page_id, Web::HTML::CrossProcessId navigable_id, Web::NavigationTarget target, URL::URL url, Utf16String navigation_id, Optional<Web::HTML::NavigationStartRequest> start_request) override;
    virtual void did_complete_navigation_unload_check(Web::PageId page_id, Web::HTML::CrossProcessId navigable_id, Utf16String navigation_id) override;
    virtual void did_request_navigation_population(Web::PageId page_id, Web::HTML::CrossProcessId navigable_id, Web::NavigationTarget target, Web::HTML::NavigationPopulationRequest) override;
    virtual void did_request_navigation_of_navigable(Web::PageId page_id, Web::HTML::CrossProcessId navigable_id, Web::HTML::PreparedNavigationDescriptor) override;
    virtual void did_post_message_to_navigable(Web::PageId page_id, Web::HTML::CrossProcessId navigable_id, Web::HTML::PostedMessageDescriptor) override;
    virtual void did_request_close_of_traversable(Web::PageId page_id, Web::HTML::CrossProcessId navigable_id, Web::HTML::CrossProcessId source_navigable_id) override;
    virtual void did_finish_navigation_params_creation(Web::PageId page_id, Web::HTML::CrossProcessId navigable_id, Utf16String navigation_id, Optional<Web::HTML::NavigationPopulationResult>) override;
    virtual void did_finish_history_navigation_params_creation(Web::PageId page_id, Web::HTML::CrossProcessId operation_id, Web::HTML::HistoryNavigationPopulation) override;
    virtual void did_fail_navigation_population(Web::PageId page_id, Web::HTML::CrossProcessId navigable_id, Utf16String navigation_id) override;
    virtual void did_change_replicated_navigable_state(Web::PageId page_id, Web::HTML::CrossProcessId navigable_id, Web::HTML::ReplicatedNavigableState) override;
    virtual void did_completely_finish_loading(Web::PageId page_id, Web::HTML::CrossProcessId navigable_id) override;
    virtual void did_change_navigable_container_state(Web::PageId page_id, Web::HTML::CrossProcessId navigable_id, Web::HTML::ReplicatedContainerState) override;
    virtual void did_create_child_frame(Web::PageId page_id, Web::HTML::CrossProcessId parent_frame_id, Web::HTML::CrossProcessId frame_id, Web::HTML::ReplicatedNavigableState replicated_state) override;
    virtual void did_update_child_frame_viewport(Web::PageId page_id, Web::HTML::CrossProcessId frame_id, Web::DevicePixelRect viewport_rect, double device_pixel_ratio) override;
    virtual void did_destroy_child_frame(Web::PageId page_id, Web::HTML::CrossProcessId frame_id) override;
    virtual void did_finish_loading(Web::PageId page_id, Web::HTML::CrossProcessId navigable_id, Optional<Utf16String>) override;
    virtual void did_request_refresh(Web::PageId page_id) override;
    virtual void did_request_cursor_change(Web::PageId page_id, Gfx::Cursor) override;
    virtual void did_change_title(Web::PageId page_id, Utf16String) override;
    virtual void did_update_editing_history_state(Web::PageId page_id, bool can_undo, bool can_redo) override;
    virtual void did_request_tooltip_override(Web::PageId page_id, Gfx::IntPoint, ByteString) override;
    virtual void did_stop_tooltip_override(Web::PageId page_id) override;
    virtual void did_enter_tooltip_area(Web::PageId page_id, ByteString) override;
    virtual void did_leave_tooltip_area(Web::PageId page_id) override;
    virtual void did_hover_link(Web::PageId page_id, URL::URL) override;
    virtual void did_unhover_link(Web::PageId page_id) override;
    virtual void did_click_link(Web::PageId page_id, URL::URL, ByteString, unsigned) override;
    virtual void did_middle_click_link(Web::PageId page_id, URL::URL, ByteString, unsigned) override;
    virtual void did_request_external_url(Web::PageId page_id, URL::URL, URL::Origin, bool has_transient_activation) override;
    virtual Messages::WebContentClient::DidStartDownloadWithoutRequestResponse did_start_download_without_request(Web::PageId page_id, URL::URL, ByteString suggested_filename, Optional<u64> total_size) override;
    virtual Messages::WebContentClient::DidStartDownloadResponse did_start_download(Web::PageId page_id, Web::HTML::CrossProcessId navigable_id, Optional<Utf16String> navigation_id, URL::URL, ByteString suggested_filename, Optional<u64> total_size, int request_server_client_id, u64 request_server_request_id, ByteBuffer initial_data) override;
    virtual void did_receive_download_data(Web::PageId page_id, u64 download_id, ByteBuffer data) override;
    virtual void did_finish_download(Web::PageId page_id, u64 download_id) override;
    virtual void did_fail_download(Web::PageId page_id, u64 download_id, String error) override;
    virtual void did_request_context_menu(Web::PageId page_id, Gfx::IntPoint, Web::ContextMenuForInputEventsTarget) override;
    virtual void did_request_link_context_menu(Web::PageId page_id, Gfx::IntPoint, URL::URL, ByteString, unsigned) override;
    virtual void did_request_image_context_menu(Web::PageId page_id, Gfx::IntPoint, URL::URL, ByteString, unsigned, Optional<Gfx::ShareableBitmap>) override;
    virtual void did_request_media_context_menu(Web::PageId page_id, Gfx::IntPoint, ByteString, unsigned, Web::Page::MediaContextMenu) override;
    virtual void did_get_source(Web::PageId page_id, URL::URL, URL::URL, Utf16String) override;
    virtual void did_inspect_dom_tree(Web::PageId page_id, String) override;
    virtual void did_inspect_storage(Web::PageId page_id, u64 request_id, String) override;
    virtual void did_inspect_dom_node(Web::PageId page_id, DOMNodeProperties) override;
    virtual void did_inspect_grid_layouts(Web::PageId page_id, String) override;
    virtual void did_inspect_current_grid(Web::PageId page_id, String) override;
    virtual void did_inspect_current_flexbox(Web::PageId page_id, String) override;
    virtual void did_inspect_indexed_database(Web::PageId page_id, u64 request_id, String) override;
    virtual void did_inspect_accessibility_tree(Web::PageId page_id, String) override;
    virtual void did_get_hovered_node_id(Web::PageId page_id, Web::UniqueNodeID node_id) override;
    virtual void did_get_node_id_at_position(Web::PageId page_id, u64 request_id, Web::UniqueNodeID node_id) override;
    virtual void did_finish_editing_dom_node(Web::PageId page_id, Optional<Web::UniqueNodeID> node_id) override;
    virtual void did_mutate_dom(Web::PageId page_id, Mutation) override;
    virtual void did_get_dom_node_html(Web::PageId page_id, String html) override;
    virtual void did_list_style_sheets(Web::PageId page_id, Vector<Web::CSS::StyleSheetIdentifier> stylesheets) override;
    virtual void did_get_style_sheet_source(Web::PageId page_id, Web::CSS::StyleSheetIdentifier identifier, URL::URL, Utf16String source) override;
    virtual void did_list_devtools_sources(Web::PageId page_id, u64 request_id, Vector<Web::HTML::ScriptRegistry::Description> sources) override;
    virtual void did_get_devtools_source(Web::PageId page_id, Web::HTML::ScriptRegistry::Identifier source_id, Optional<Web::HTML::ScriptRegistry::Content> source) override;
    virtual void did_add_devtools_source(Web::PageId page_id, Web::HTML::ScriptRegistry::Description source) override;
    virtual void did_pause_debugger(Web::PageId page_id, DebuggerPause) override;
    virtual void did_resume_debugger(Web::PageId page_id) override;
    virtual void did_complete_debugger_breakpoint_operation(Web::PageId page_id, u64 request_id, Optional<String> error) override;
    virtual void did_get_debugger_environments(Web::PageId page_id, u64 request_id, Optional<String> error, Vector<DebuggerEnvironment>) override;
    virtual void did_evaluate_javascript_in_debugger_frame(Web::PageId page_id, u64 request_id, Optional<String> error, DebuggerEvaluationResult) override;
    virtual void did_get_debugger_object_properties(Web::PageId page_id, u64 request_id, Optional<String> error, DebuggerObjectProperties) override;
    virtual void did_get_debugger_source_positions(Web::PageId page_id, u64 request_id, Vector<DebuggerSourcePosition> positions) override;
    virtual void did_resolve_dom_node_url(Web::PageId page_id, u64 request_id, String resolved_url) override;
    virtual void did_take_screenshot(Web::PageId page_id, Gfx::ShareableBitmap screenshot) override;
    virtual void did_get_internal_page_info(Web::PageId page_id, PageInfoType, Optional<Core::AnonymousBuffer>) override;
    virtual void did_get_selected_text(Web::PageId page_id, u64 request_id, ByteString selection) override;
    virtual void did_get_selected_text_for_lookup(Web::PageId page_id, u64 request_id, Optional<DictionaryLookup> lookup) override;
    virtual void did_select_word_for_dictionary_lookup(Web::PageId page_id, u64 request_id, bool selected) override;
    virtual void did_cut_selected_text(Web::PageId page_id, u64 request_id, ByteString selection) override;
    virtual void did_execute_js_console_input(Web::PageId page_id, JsonValue) override;
    virtual void did_output_js_console_message(Web::PageId page_id, ConsoleOutput) override;
    virtual void did_start_network_request(Web::PageId page_id, u64 request_id, URL::URL, ByteString method, Vector<HTTP::Header>, ByteBuffer request_body, Optional<String> initiator_type, String referrer_policy, bool is_navigation_request, Web::Fetch::Infrastructure::Request::Priority) override;
    virtual void did_receive_network_response_headers(Web::PageId page_id, u64 request_id, u32 status_code, Optional<String> reason_phrase, Vector<HTTP::Header>, Requests::CameFromCache) override;
    virtual void did_receive_network_response_body(Web::PageId page_id, u64 request_id, ByteBuffer data) override;
    virtual void did_finish_network_request(Web::PageId page_id, u64 request_id, u64 body_size, Requests::RequestTimingInfo, Optional<Requests::NetworkError>) override;
    virtual void did_change_favicon(Web::PageId page_id, Gfx::ShareableBitmap) override;
    virtual void did_request_alert(Web::PageId page_id, Utf16String) override;
    virtual void did_request_confirm(Web::PageId page_id, Utf16String) override;
    virtual void did_request_prompt(Web::PageId page_id, Utf16String, Utf16String) override;
    virtual void did_request_set_prompt_text(Web::PageId page_id, Utf16String message) override;
    virtual void did_request_accept_dialog(Web::PageId page_id) override;
    virtual void did_request_dismiss_dialog(Web::PageId page_id) override;
    virtual void did_request_document_cookie_version_index(Web::PageId page_id, i64 document_id, String domain) override;
    virtual Messages::WebContentClient::DidRequestAllCookiesWebdriverResponse did_request_all_cookies_webdriver(URL::URL) override;
    virtual Messages::WebContentClient::DidRequestAllCookiesCookiestoreResponse did_request_all_cookies_cookiestore(URL::URL) override;
    virtual Messages::WebContentClient::DidRequestNamedCookieResponse did_request_named_cookie(URL::URL, String) override;
    virtual Messages::WebContentClient::DidRequestCookieResponse did_request_cookie(Web::PageId page_id, URL::URL, HTTP::Cookie::Source) override;
    virtual void did_set_cookie(URL::URL, HTTP::Cookie::ParsedCookie, HTTP::Cookie::Source) override;
    virtual void did_update_cookie(HTTP::Cookie::Cookie) override;
    virtual void did_request_delete_all_cookies(Web::PageId page_id, u64 request_id, URL::URL) override;
    virtual void did_store_hsts_policy(String, HTTP::HSTS::ParsedHSTSPolicy) override;
    virtual Messages::WebContentClient::DidIsKnownHstsHostResponse did_is_known_hsts_host(String) override;
    virtual Messages::WebContentClient::DidLoseRequestServerConnectionResponse did_lose_request_server_connection() override;
    virtual Messages::WebContentClient::DidRequestStorageItemResponse did_request_storage_item(Web::PageId page_id, Web::StorageAPI::StorageEndpointType storage_endpoint, String storage_key, Utf16String bottle_key) override;
    virtual Messages::WebContentClient::DidSetStorageItemResponse did_set_storage_item(Web::PageId page_id, Web::StorageAPI::StorageEndpointType storage_endpoint, String storage_key, Utf16String bottle_key, Utf16String value) override;
    virtual void did_remove_storage_item(Web::PageId page_id, Web::StorageAPI::StorageEndpointType storage_endpoint, String storage_key, Utf16String bottle_key) override;
    virtual Messages::WebContentClient::DidRequestStorageKeysResponse did_request_storage_keys(Web::PageId page_id, Web::StorageAPI::StorageEndpointType storage_endpoint, String storage_key) override;
    virtual void did_clear_storage(Web::PageId page_id, Web::StorageAPI::StorageEndpointType storage_endpoint, String storage_key) override;
    virtual Messages::WebContentClient::DidRequestStorageUsageResponse did_request_storage_usage(Web::PageId page_id, String storage_key) override;
    virtual void did_change_storage_item(Web::PageId page_id, Web::StorageAPI::StorageEndpointType storage_endpoint, String url, Optional<Utf16String> key, Optional<Utf16String> old_value, Optional<Utf16String> new_value) override;
    virtual void did_update_indexed_database(Web::PageId page_id, String update) override;
    virtual void did_post_broadcast_channel_message(Web::PageId page_id, Web::HTML::BroadcastChannelMessage message) override;
    virtual Messages::WebContentClient::DidRequestNewWebViewResponse did_request_new_web_view(Web::PageId page_id, Web::HTML::ActivateTab, Web::HTML::WebViewHints, Optional<Web::HTML::CrossProcessId> opener_navigable_id, Optional<URL::URL> opener_base_url, Utf16String target_name) override;
    virtual void did_request_activate_tab(Web::PageId page_id) override;
    virtual void did_close_browsing_context(Web::PageId page_id) override;
    virtual void did_change_needs_beforeunload_check(Web::PageId page_id, bool needs_beforeunload_check) override;
    virtual void did_consume_user_activation(Web::PageId page_id, Web::HTML::UserActivationConsumption) override;
    virtual void webdriver_user_prompt_handling_complete(Web::PageId page_id, u64 request_id, Web::WebDriver::Response response) override;
    virtual void webdriver_command_complete(Web::PageId page_id, u64 command_id, Web::WebDriver::Response response) override;
    virtual void did_update_resource_count(Web::PageId page_id, i32 count_waiting) override;
    virtual void did_request_restore_window(Web::PageId page_id) override;
    virtual void did_request_reposition_window(Web::PageId page_id, Gfx::IntPoint, u64 completion_id) override;
    virtual void did_request_resize_window(Web::PageId page_id, Gfx::IntSize, u64 completion_id) override;
    virtual void did_request_maximize_window(Web::PageId page_id, u64 completion_id) override;
    virtual void did_request_minimize_window(Web::PageId page_id) override;
    virtual void did_request_fullscreen_window(Web::PageId page_id) override;
    virtual void did_request_exit_fullscreen(Web::PageId page_id) override;
    virtual void did_request_file(Web::PageId page_id, ByteString path, i32) override;
    virtual void did_request_color_picker(Web::PageId page_id, Color current_color) override;
    virtual void did_request_geolocation_position(Web::PageId page_id, u64 request_id) override;
    virtual void did_cancel_geolocation_position_request(Web::PageId page_id, u64 request_id) override;
    virtual void did_start_geolocation_position_watch(Web::PageId page_id, u64 request_id) override;
    virtual void did_stop_geolocation_position_watch(Web::PageId page_id, u64 request_id) override;
    virtual void did_request_file_picker(Web::PageId page_id, Web::HTML::FileFilter accepted_file_types, Web::HTML::AllowMultipleFiles) override;
    virtual void did_request_select_dropdown(Web::PageId page_id, Gfx::IntPoint content_position, i32 minimum_width, Vector<Web::HTML::SelectItem> items) override;
    virtual void did_finish_handling_input_event(Web::PageId page_id, u64 event_id, Web::EventResult event_result) override;
    virtual void did_update_input_method_state(Web::PageId page_id, Optional<Web::DevicePixelRect> caret_rect, bool is_enabled, i32 cursor_position, i32 anchor_position, Utf16String text_before_cursor, Utf16String text_after_cursor) override;
    virtual void did_set_browser_zoom(Web::PageId page_id, double factor) override;
    virtual void did_find_in_page(Web::PageId page_id, size_t current_match_index, Optional<size_t> total_match_count) override;
    virtual void did_change_theme_color(Web::PageId page_id, Gfx::Color color) override;
    virtual void did_change_background_color(Web::PageId page_id, Gfx::Color color) override;
    virtual void did_insert_clipboard_item(Web::PageId page_id, Web::Clipboard::SystemClipboardItem, String presentation_style) override;
    virtual void did_request_clipboard_entries(Web::PageId page_id, u64 request_id) override;
    virtual void did_request_primary_paste(Web::PageId page_id) override;
    virtual void did_update_primary_selection(Web::PageId page_id, String) override;
    virtual void did_change_audio_play_state(Web::PageId page_id, Web::HTML::AudioPlayState) override;
    virtual void did_change_screen_wake_lock_state(Web::PageId page_id, Web::ScreenWakeLockState) override;
    virtual void did_update_session_history_entry_navigation_api_state(Web::PageId page_id, Web::HTML::CrossProcessId navigable_id, Web::HTML::SessionHistoryEntryIdentity entry_identity, Web::HTML::StorageSerializationRecord navigation_api_state) override;
    virtual void did_update_session_history_entry_scroll_restoration_mode(Web::PageId page_id, Web::HTML::CrossProcessId navigable_id, Web::HTML::SessionHistoryEntryIdentity entry_identity, Web::HTML::ScrollRestorationMode scroll_restoration_mode) override;
    virtual void did_update_session_history_entry_document_state_navigable_target_name(Web::PageId page_id, Web::HTML::CrossProcessId navigable_id, Web::HTML::SessionHistoryEntryIdentity entry_identity, Utf16String navigable_target_name) override;
    virtual void did_set_session_history_entry_document_state_reload_pending(Web::PageId page_id, Web::HTML::CrossProcessId navigable_id, Utf16String navigation_api_key, bool reload_pending) override;
    virtual void did_request_set_system_focus(Web::PageId page_id, bool has_system_focus) override;
    virtual void did_change_focused_navigable(Web::PageId page_id, Web::HTML::CrossProcessId navigable_id) override;
    virtual void did_request_set_system_visibility_state(Web::PageId page_id, Web::HTML::VisibilityState) override;
    virtual void request_history_operation(Web::PageId page_id, Web::HTML::CrossProcessId operation_id, Web::HistoryOperationParameters) override;
    virtual void history_operation_ready(Web::PageId page_id, Web::HTML::CrossProcessId operation_id, Web::HistoryOperationReadyResult) override;
    virtual void history_step_unload_cancelation_result(Web::PageId page_id, Web::HTML::CrossProcessId operation_id, Web::HTML::HistoryStepResult result, Web::HTML::UnloadPromptShown unload_prompt_shown) override;
    virtual void beforeunload_check_result(Web::PageId page_id, Web::HTML::CrossProcessId operation_id, Web::HTML::HistoryStepResult result, Web::HTML::UnloadPromptShown unload_prompt_shown) override;
    virtual void changing_navigable_history_job_ready(Web::PageId page_id, Web::HTML::CrossProcessId operation_id, Web::HTML::CrossProcessId navigable_id, Web::HTML::ChangingNavigableHistoryStepJobDisposition disposition, Web::HTML::UnloadDisplayedDocument unload_displayed_document) override;
    virtual void changing_navigable_unload_preparation_complete(Web::PageId page_id, Web::HTML::CrossProcessId operation_id, Web::HTML::CrossProcessId navigable_id) override;
    virtual void descendant_unload_task_complete(Web::PageId page_id, Web::HTML::CrossProcessId unload_id, Web::HTML::CrossProcessId navigable_id) override;
    virtual void request_child_navigable_unload(Web::PageId page_id, Web::HTML::CrossProcessId navigable_id) override;
    virtual void request_unload_check(Web::PageId page_id, Web::HTML::CrossProcessId navigable_id, Web::HTML::CrossProcessId check_id) override;
    virtual void request_navigable_document_abort(Web::PageId page_id, Web::HTML::CrossProcessId navigable_id) override;
    virtual void request_navigable_document_unfullscreen(Web::PageId page_id, Web::HTML::CrossProcessId navigable_id) override;
    virtual void changing_navigable_continuation_applied(Web::PageId page_id, Web::HTML::CrossProcessId operation_id, Web::HTML::CrossProcessId navigable_id, Optional<Web::HTML::ReplicatedNavigableState> activated_navigable_state, Optional<Web::HTML::SessionHistoryEntryPersistedState> previous_entry_persisted_state) override;
    virtual void nonchanging_navigable_history_state_updated(Web::PageId page_id, Web::HTML::CrossProcessId operation_id, Web::HTML::CrossProcessId navigable_id) override;
    virtual Messages::WebContentClient::StartWorkerAgentResponse start_worker_agent(Web::PageId page_id, Web::HTML::WorkerAgentStartRequest request) override;
    virtual void close_worker_agent(Web::PageId page_id, Web::HTML::WorkerAgentId agent_id, Web::HTML::WorkerAgentOwnerToken owner_token) override;

    Optional<ViewImplementation&> view_for_page_id(Web::PageId, SourceLocation = SourceLocation::current());
    Optional<ViewImplementation&> owning_view_for_page_id(Web::PageId);

    void remember_compositor_context(Web::Compositor::CompositorContextId, Optional<Web::PageId> page_id);
    bool is_renderer_owned_download(Web::PageId page_id, u64 download_id) const;
    void forget_renderer_owned_download(u64 download_id);
    void fail_renderer_owned_downloads();

    RefPtr<WebContentTestClient> m_test_connection;

    IsPrivate m_is_private { IsPrivate::No };
    RefPtr<BrowsingSession> m_session;
    bool m_process_lost { false };
    bool m_rejected_ipc { false };

    HashMap<Web::PageId, NonnullRawPtr<ViewImplementation>> m_views;
    HashMap<Web::PageId, WeakPtr<CanonicalNavigable>> m_embedded_pages;
    HashMap<Web::PageId, bool> m_needs_beforeunload_check_by_page;
    HashTable<Web::PageId> m_detached_pages_pending_close;
    // Every page ID the UI process has handed to this connection. A page stays in the set once it closes,
    // because messages the connection sent while it had the page can arrive after the page is gone.
    HashTable<Web::PageId> m_assigned_pages;
    HashMap<Web::Compositor::CompositorContextId, Optional<Web::PageId>> m_compositor_contexts;
    HashMap<u64, Web::PageId> m_renderer_owned_downloads;
    HashMap<Web::PageId, String> m_history_recorded_urls_for_current_load;
    Optional<i32> m_compositor_connection_id;
    Optional<Web::PageId> m_unassigned_initial_page_id;
    Web::HTML::CrossProcessId m_root_navigable_id;
    Optional<Web::HTML::SessionHistoryEntryDescriptor> m_initial_top_level_history_entry;

    ProcessHandle m_process_handle;
    RefPtr<Core::Timer> m_detached_page_close_timer;

    RefPtr<WebUI> m_web_ui;

    static HashTable<WebContentClient*>& clients();
};

template<CallableAs<IterationDecision, WebContentClient&> Callback>
void WebContentClient::for_each_client(Callback callback)
{
    for (auto& it : clients()) {
        if (callback(*it) == IterationDecision::Break)
            return;
    }
}

}
