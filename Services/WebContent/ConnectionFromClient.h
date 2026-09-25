/*
 * Copyright (c) 2018-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/Queue.h>
#include <AK/RefPtr.h>
#include <AK/SourceLocation.h>
#include <LibCompositing/InputEvent.h>
#include <LibCompositing/PageId.h>
#include <LibCompositing/Types.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibGC/Root.h>
#include <LibIPC/ConnectionFromClient.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/Navigation.h>
#include <LibWeb/Bindings/NavigationType.h>
#include <LibWeb/CSS/PreferredColorScheme.h>
#include <LibWeb/CSS/PreferredContrast.h>
#include <LibWeb/CSS/PreferredMotion.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/AutoplayPolicy.h>
#include <LibWeb/HTML/SessionHistoryEntry.h>
#include <LibWeb/HTML/WorkerAgentTypes.h>
#include <LibWeb/Loader/FileRequest.h>
#include <LibWeb/Page/EventResult.h>
#include <LibWeb/Page/QueuedInputEvent.h>
#include <LibWeb/Page/ViewportIsFullscreen.h>
#include <LibWeb/Platform/Timer.h>
#include <LibWebView/DOMNodeProperties.h>
#include <LibWebView/Debugger.h>
#include <LibWebView/Forward.h>
#include <LibWebView/Geolocation.h>
#include <LibWebView/PageInfo.h>
#include <WebContent/Forward.h>
#include <WebContent/WebContentClientEndpoint.h>
#include <WebContent/WebContentConsoleClient.h>
#include <WebContent/WebContentServerEndpoint.h>

namespace Gfx {

class SharedFontProvider;

}

namespace WebContent {

class ConnectionFromClient final
    : public IPC::ConnectionFromClient<WebContentClientEndpoint, WebContentServerEndpoint> {
    C_OBJECT(ConnectionFromClient);

    friend class TestConnection;

public:
    ~ConnectionFromClient() override;

    virtual void die() override;

    void request_file(Compositing::PageId page_id, Web::FileRequest);

    PageHost& page_host() { return *m_page_host; }
    PageHost const& page_host() const { return *m_page_host; }
    WebView::CompositorConnection* compositor_process_connection() const;
    void did_destroy_compositor_context(Compositing::CompositorContextId);

    Function<void(IPC::TransportHandle const&)> on_request_server_connection;
    Function<void(IPC::TransportHandle const&)> on_image_decoder_connection;
#if defined(HAVE_WASM_COMPILER_SERVICE)
    Function<void(IPC::TransportHandle)> on_wasm_compiler_connection;
#endif

    // Null outside test mode: the UI process connects the test endpoint only when it runs tests.
    TestConnection* test_connection();

    Queue<Web::QueuedInputEvent>& input_event_queue() { return m_input_event_queue; }
    void update_input_method_state(Compositing::PageId page_id);

private:
    ConnectionFromClient(NonnullOwnPtr<IPC::Transport>, bool enable_test_mode);

    Optional<PageClient&> page(Compositing::PageId index, SourceLocation = SourceLocation::current());
    Optional<PageClient const&> page(Compositing::PageId index, SourceLocation = SourceLocation::current()) const;

    virtual Messages::WebContentServer::InitTransportResponse init_transport(int peer_pid) override;
    virtual void set_font_catalog(IPC::File, u64 size, u64 generation) override;
    virtual void initialize(Compositing::PageId initial_page_id, Vector<Web::HTML::RemoteNavigableDescriptor> remote_navigables, Web::HTML::CrossProcessId root_navigable_id, Web::HTML::CrossProcessIdAllocator cross_process_id_allocator, Web::HTML::SessionHistoryEntryDescriptor initial_history_entry, Web::HTML::VisibilityState system_visibility_state) override;
    virtual void create_representing_page(Compositing::PageId page_id, Vector<Web::HTML::RemoteNavigableDescriptor> remote_navigables) override;
    virtual void create_embedded_page(Compositing::PageId page_id, Vector<Web::HTML::RemoteNavigableDescriptor> remote_navigables, Web::HTML::CrossProcessId root_navigable_id, Web::HTML::SessionHistoryEntryDescriptor initial_history_entry, Web::HTML::VisibilityState system_visibility_state) override;
    virtual void continue_history_navigation_population(Compositing::PageId page_id, Web::HTML::CrossProcessId operation_id, Web::HTML::SessionHistoryEntryDescriptor target_entry, Optional<Web::Bindings::NavigationType>, Web::HTML::HistoryNavigationPopulation) override;
    virtual void insert_remote_navigable(Compositing::PageId page_id, Web::HTML::RemoteNavigableDescriptor) override;
    virtual void remove_remote_navigable(Compositing::PageId page_id, Web::HTML::CrossProcessId navigable_id) override;
    virtual void update_remote_navigable(Compositing::PageId page_id, Web::HTML::CrossProcessId navigable_id, Web::HTML::ReplicatedNavigableState) override;
    virtual void content_navigable_completely_finished_loading(Compositing::PageId page_id, Web::HTML::CrossProcessId navigable_id) override;
    virtual void update_local_root_container_state(Compositing::PageId page_id, Web::HTML::CrossProcessId navigable_id, Web::HTML::ReplicatedContainerState) override;
    virtual void close_server() override;
    virtual Messages::WebContentServer::GetWindowHandleResponse get_window_handle(Compositing::PageId page_id) override;
    virtual void set_window_handle(Compositing::PageId page_id, String handle) override;
    virtual void run_webdriver_command(Compositing::PageId page_id, u64 command_id, Optional<Web::HTML::CrossProcessId> navigable_id, String name, JsonValue payload, Vector<String> arguments) override;
    virtual void set_webdriver_session_config(Compositing::PageId page_id, Web::WebDriver::UserPromptHandler user_prompt_handler, Web::WebDriver::PageLoadStrategy page_load_strategy, bool strict_file_interactability, JsonValue timeouts) override;
    virtual void run_webdriver_user_prompt_handling(Compositing::PageId page_id, u64 request_id) override;
    virtual void did_handle_webdriver_mouse_event(Compositing::PageId page_id, u64 request_id) override;
    virtual void connect_to_web_ui(Compositing::PageId page_id, IPC::TransportHandle handle) override;
    virtual void connect_to_request_server(IPC::TransportHandle handle) override;
    virtual void connect_to_test_endpoint(IPC::TransportHandle handle) override;
    virtual void connect_to_image_decoder(IPC::TransportHandle handle) override;
    virtual void connect_to_wasm_compiler(IPC::TransportHandle handle) override;
    virtual void connect_to_compositor_process(IPC::TransportHandle handle) override;
    virtual void set_site_compatibility_data(JsonValue data) override;
    virtual void compositor_process_reconnected() override;
    virtual void update_system_theme(Compositing::PageId page_id, Core::AnonymousBuffer) override;
    virtual void update_screen_rects(Compositing::PageId page_id, Vector<Compositing::DevicePixelRect>, u32) override;
    virtual void populate_navigation(Compositing::PageId page_id, Web::HTML::NavigationPopulationRequest, Web::HTML::NavigationPopulationResult) override;
    virtual void reload(Compositing::PageId page_id) override;
    virtual void stop_loading(Compositing::PageId page_id) override;
    virtual void cancel_download(Compositing::PageId page_id, u64 download_id) override;
    virtual void set_ongoing_navigation(Compositing::PageId page_id, Web::HTML::CrossProcessId navigable_id, Utf16String navigation_id) override;
    virtual void navigate_to_a_fragment(Compositing::PageId page_id, Web::HTML::CrossProcessId navigable_id, URL::URL url, Web::HTML::HistoryHandlingBehavior, Web::HTML::UserNavigationInvolvement, Utf16String navigation_id) override;
    virtual void navigate_to_a_javascript_url(Compositing::PageId page_id, Web::HTML::CrossProcessId navigable_id, URL::URL url, Web::HTML::HistoryHandlingBehavior, URL::Origin initiator_origin, Web::HTML::NavigationSourceSnapshot source_snapshot_params, Web::HTML::UserNavigationInvolvement, Web::ContentSecurityPolicy::Directives::Directive::NavigationType csp_navigation_type, Utf16String navigation_id) override;
    virtual void run_navigation_unload_check(Compositing::PageId page_id, Web::HTML::CrossProcessId navigable_id, Utf16String navigation_id, Web::HTML::UnloadPromptShown) override;
    virtual void create_navigation_params(Compositing::PageId page_id, Web::HTML::NavigationPopulationRequest) override;
    virtual void cancel_navigation_params_creation(Compositing::PageId page_id, Web::HTML::CrossProcessId navigable_id, Utf16String navigation_id) override;
    virtual void navigate_navigable(Compositing::PageId page_id, Web::HTML::CrossProcessId navigable_id, Web::HTML::PreparedNavigationDescriptor) override;
    virtual void deliver_posted_message(Compositing::PageId page_id, Web::HTML::CrossProcessId navigable_id, Web::HTML::PostedMessageDescriptor) override;
    virtual void close_traversable_from_script(Compositing::PageId page_id, Web::HTML::CrossProcessId navigable_id, Web::HTML::CrossProcessId source_navigable_id) override;
    virtual void run_focusing_steps_for_navigable(Compositing::PageId page_id, Web::HTML::CrossProcessId navigable_id, Web::HTML::FocusTrigger) override;
    virtual void focus_window_of_navigable(Compositing::PageId page_id, Web::HTML::CrossProcessId navigable_id) override;
    virtual void set_opener_of_navigable(Compositing::PageId page_id, Web::HTML::CrossProcessId navigable_id, Web::HTML::CrossProcessId opener_navigable_id) override;
    virtual void begin_hosting_navigable(Compositing::PageId page_id, Web::HTML::CrossProcessId navigable_id, Web::HTML::SessionHistoryEntryDescriptor, Web::HTML::VisibilityState) override;
    virtual void discard_provisional_navigable(Compositing::PageId page_id, Web::HTML::CrossProcessId navigable_id) override;
    virtual void stop_hosting_navigable(Compositing::PageId page_id, Web::HTML::CrossProcessId navigable_id, Web::HTML::ReplicatedNavigableState) override;
    virtual void host_navigable(Compositing::PageId page_id, Web::HTML::CrossProcessId navigable_id, Web::HTML::SessionHistoryEntryDescriptor, Web::HTML::VisibilityState) override;
    virtual void set_hosted_root_viewport(Compositing::PageId page_id, Web::HTML::CrossProcessId navigable_id, Compositing::DevicePixelSize, Compositing::DevicePixelRect viewport_intersection, double device_pixel_ratio) override;
    virtual void history_operation_started(Compositing::PageId page_id, Web::HTML::CrossProcessId operation_id) override;
    virtual void reconstruct_child_navigable_history(Compositing::PageId page_id, Web::HTML::CrossProcessId navigable_id, Web::ReconstructedChildNavigation navigation) override;
    virtual void run_history_step_unload_cancelation_job(Compositing::PageId page_id, Web::HTML::CrossProcessId operation_id, Web::HTML::SessionHistoryEntryDescriptor target_entry, Vector<Web::HTML::CrossProcessId> navigables_crossing_documents, Web::HTML::UserNavigationInvolvement user_involvement) override;
    virtual void run_beforeunload_check(Compositing::PageId page_id, Web::HTML::CrossProcessId operation_id, Vector<Web::HTML::CrossProcessId> navigable_ids, Web::HTML::UnloadPromptShown unload_prompt_shown) override;
    virtual void discard_embedded_page(Compositing::PageId page_id) override;
    virtual void queue_navigation_api_state_clear_task(Compositing::PageId page_id, Web::HTML::CrossProcessId operation_id, Web::HTML::CrossProcessId navigable_id) override;
    virtual void run_changing_navigable_history_job(Compositing::PageId page_id, Web::HTML::CrossProcessId operation_id, Web::HTML::CrossProcessId navigable_id, Web::HTML::SessionHistoryEntryDescriptor target_entry, Web::HTML::UserNavigationInvolvement user_involvement, Optional<Web::Bindings::NavigationType> navigation_type, Web::HTML::TraversalYieldsTo traversal_yields_to, Optional<Utf16String> canceled_navigation_id) override;
    virtual void prepare_changing_navigable_for_unload(Compositing::PageId page_id, Web::HTML::CrossProcessId operation_id, Web::HTML::CrossProcessId navigable_id) override;
    virtual void apply_changing_navigable_continuation(Compositing::PageId page_id, Web::HTML::CrossProcessId operation_id, Web::HTML::CrossProcessId navigable_id, u64 script_history_length, u64 script_history_index, Vector<Web::HTML::SessionHistoryEntryDescriptor> entries_for_navigation_api, Web::HTML::VisibilityState system_visibility_state, Web::HTML::UnloadDisplayedDocument unload_displayed_document) override;
    virtual void run_descendant_unload_task(Compositing::PageId page_id, Web::HTML::CrossProcessId unload_id, Web::HTML::CrossProcessId navigable_id, Web::HTML::ChildNavigableDestruction, Web::HTML::StopHostingAfterUnload) override;
    virtual void continue_child_navigable_destruction(Compositing::PageId page_id, Web::HTML::CrossProcessId navigable_id) override;
    virtual void abort_navigable_document(Compositing::PageId page_id, Web::HTML::CrossProcessId navigable_id) override;
    virtual void unload_check_result(Compositing::PageId page_id, Web::HTML::CrossProcessId check_id, Web::HTML::HistoryStepResult) override;
    virtual void unfullscreen_navigable_document(Compositing::PageId page_id, Web::HTML::CrossProcessId navigable_id) override;
    virtual void fullscreen_navigable_container(Compositing::PageId page_id, Web::HTML::CrossProcessId navigable_id, Web::HTML::CrossProcessId requesting_navigable_id, Web::Fullscreen::RequestType request_type) override;
    virtual void container_fullscreen_complete(Compositing::PageId page_id, Web::HTML::CrossProcessId navigable_id) override;
    virtual void unfullscreen_navigable_container(Compositing::PageId page_id, Web::HTML::CrossProcessId navigable_id) override;
    virtual void container_unfullscreen_complete(Compositing::PageId page_id, Web::HTML::CrossProcessId navigable_id) override;
    virtual void run_traversable_close_unload_task(Compositing::PageId page_id, Web::HTML::CrossProcessId operation_id) override;
    virtual void update_nonchanging_navigable_history_state(Compositing::PageId page_id, Web::HTML::CrossProcessId operation_id, Web::HTML::CrossProcessId navigable_id, u64 script_history_length, u64 script_history_index) override;
    virtual void complete_history_operation(Compositing::PageId page_id, Web::HTML::CrossProcessId operation_id, Web::HTML::HistoryStepResult result, Optional<i32> committed_step, u64 session_history_entry_count) override;
    virtual void set_viewport(Compositing::PageId page_id, Compositing::DevicePixelSize, double device_pixel_ratio, Web::ViewportIsFullscreen is_fullscreen) override;
    virtual void set_viewport_is_fullscreen(Compositing::PageId page_id, Web::ViewportIsFullscreen is_fullscreen) override;
    virtual void key_event(Compositing::PageId page_id, Compositing::KeyEvent) override;
    virtual void mouse_event(Compositing::PageId page_id, Compositing::MouseEvent) override;
    virtual void mouse_event_in_hosted_root(Compositing::PageId page_id, Web::HTML::CrossProcessId navigable_id, Compositing::MouseEvent) override;
    virtual void drag_event(Compositing::PageId page_id, Web::DragEvent) override;
    virtual void pinch_event(Compositing::PageId page_id, Compositing::PinchEvent) override;
    virtual void debug_request(Compositing::PageId page_id, ByteString, ByteString) override;
    virtual void get_source(Compositing::PageId page_id) override;
    virtual void inspect_dom_tree(Compositing::PageId page_id) override;
    virtual void inspect_storage(Compositing::PageId page_id, Web::StorageAPI::StorageEndpointType storage_endpoint, u64 request_id) override;
    virtual Messages::WebContentServer::SetSessionStorageItemResponse set_session_storage_item(Compositing::PageId page_id, Utf16String key, Utf16String value) override;
    virtual Messages::WebContentServer::RemoveSessionStorageItemResponse remove_session_storage_item(Compositing::PageId page_id, Utf16String key) override;
    virtual Messages::WebContentServer::ClearSessionStorageResponse clear_session_storage(Compositing::PageId page_id) override;
    virtual void inspect_dom_node(Compositing::PageId page_id, WebView::DOMNodeProperties::Type, Compositing::UniqueNodeID node_id, Optional<Web::CSS::PseudoElement> pseudo_element, JsonValue options) override;
    virtual void inspect_grid_layouts(Compositing::PageId page_id, Compositing::UniqueNodeID root_node_id) override;
    virtual void inspect_current_grid(Compositing::PageId page_id, Compositing::UniqueNodeID node_id) override;
    virtual void inspect_current_flexbox(Compositing::PageId page_id, Compositing::UniqueNodeID node_id, bool only_look_at_parents) override;
    virtual void inspect_indexed_database_storage(Compositing::PageId page_id, u64 request_id) override;
    virtual void inspect_indexed_database_objects(Compositing::PageId page_id, u64 request_id, String host, JsonValue names, JsonValue options) override;
    virtual void delete_indexed_database(Compositing::PageId page_id, u64 request_id, String host, String name) override;
    virtual void clear_indexed_database_object_store(Compositing::PageId page_id, u64 request_id, String host, String name) override;
    virtual void delete_indexed_database_record(Compositing::PageId page_id, u64 request_id, String host, String name) override;
    virtual void clear_inspected_dom_node(Compositing::PageId page_id) override;
    virtual void highlight_dom_node(Compositing::PageId page_id, Compositing::UniqueNodeID node_id, Optional<Web::CSS::PseudoElement> pseudo_element) override;
    virtual void highlight_flexbox(Compositing::PageId page_id, Compositing::UniqueNodeID node_id, JsonValue options) override;
    virtual void clear_flexbox_highlight(Compositing::PageId page_id, Compositing::UniqueNodeID node_id) override;
    virtual void highlight_grid(Compositing::PageId page_id, Compositing::UniqueNodeID node_id, JsonValue options) override;
    virtual void clear_grid_highlight(Compositing::PageId page_id, Compositing::UniqueNodeID node_id) override;
    virtual void inspect_accessibility_tree(Compositing::PageId page_id) override;
    virtual void get_hovered_node_id(Compositing::PageId page_id) override;
    virtual void get_node_id_at_position(Compositing::PageId page_id, u64 request_id, Compositing::DevicePixelPoint position) override;

    virtual void list_style_sheets(Compositing::PageId page_id) override;
    virtual void request_style_sheet_source(Compositing::PageId page_id, Web::CSS::StyleSheetIdentifier identifier) override;
    virtual void list_devtools_sources(Compositing::PageId page_id, u64 request_id) override;
    virtual void request_devtools_source(Compositing::PageId page_id, Web::HTML::ScriptRegistry::Identifier source_id) override;
    virtual void attach_debugger(Compositing::PageId page_id) override;
    virtual void configure_debugger(Compositing::PageId page_id, WebView::DebuggerConfiguration) override;
    virtual void detach_debugger(Compositing::PageId page_id) override;
    virtual void interrupt_debugger(Compositing::PageId page_id) override;
    virtual void resume_debugger(Compositing::PageId page_id, WebView::DebuggerResumeMode) override;
    virtual void update_debugger_blackboxing(Compositing::PageId page_id, Utf16String, Vector<WebView::DebuggerBlackboxRange>, WebView::DebuggerBlackboxingOperation) override;
    virtual void set_debugger_breakpoint(Compositing::PageId page_id, u64 request_id, WebView::DebuggerBreakpointLocation, WebView::DebuggerBreakpointOptions) override;
    virtual void remove_debugger_breakpoint(Compositing::PageId page_id, u64 request_id, WebView::DebuggerBreakpointLocation) override;
    virtual void get_debugger_environments(Compositing::PageId page_id, u64 request_id, u64 frame_id) override;
    virtual void evaluate_javascript_in_debugger_frame(Compositing::PageId page_id, u64 request_id, u64 frame_id, Utf16String source_text) override;
    virtual void get_debugger_object_properties(Compositing::PageId page_id, u64 request_id, u64 object_id) override;
    virtual void get_debugger_source_positions(Compositing::PageId page_id, u64 request_id, Web::HTML::ScriptRegistry::Identifier) override;
    virtual void resolve_dom_node_url(Compositing::PageId page_id, u64 request_id, Optional<Compositing::UniqueNodeID> node_id, String url) override;

    virtual void set_listen_for_dom_mutations(Compositing::PageId page_id, bool) override;
    virtual void did_connect_devtools_client(Compositing::PageId page_id) override;
    virtual void did_disconnect_devtools_client(Compositing::PageId page_id) override;
    virtual void get_dom_node_inner_html(Compositing::PageId page_id, Compositing::UniqueNodeID node_id) override;
    virtual void get_dom_node_outer_html(Compositing::PageId page_id, Compositing::UniqueNodeID node_id) override;
    virtual void set_dom_node_outer_html(Compositing::PageId page_id, Compositing::UniqueNodeID node_id, String html) override;
    virtual void set_dom_node_text(Compositing::PageId page_id, Compositing::UniqueNodeID node_id, String text) override;
    virtual void set_dom_node_tag(Compositing::PageId page_id, Compositing::UniqueNodeID node_id, Utf16FlyString name) override;
    virtual void add_dom_node_attributes(Compositing::PageId page_id, Compositing::UniqueNodeID node_id, Vector<WebView::Attribute> attributes) override;
    virtual void replace_dom_node_attribute(Compositing::PageId page_id, Compositing::UniqueNodeID node_id, Utf16FlyString name, Vector<WebView::Attribute> replacement_attributes) override;
    virtual void create_child_element(Compositing::PageId page_id, Compositing::UniqueNodeID node_id) override;
    virtual void create_child_text_node(Compositing::PageId page_id, Compositing::UniqueNodeID node_id) override;
    virtual void insert_dom_node_before(Compositing::PageId page_id, Compositing::UniqueNodeID node_id, Compositing::UniqueNodeID parent_node_id, Optional<Compositing::UniqueNodeID> sibling_node_id) override;
    virtual void clone_dom_node(Compositing::PageId page_id, Compositing::UniqueNodeID node_id) override;
    virtual void remove_dom_node(Compositing::PageId page_id, Compositing::UniqueNodeID node_id) override;

    virtual void set_content_blockers(Core::AnonymousBuffer patterns) override;
    virtual void set_autoplay_settings(Compositing::PageId page_id, Web::HTML::AutoplayPolicy policy, Vector<Utf16String> allowlist) override;
    virtual void set_preferred_color_scheme(Compositing::PageId page_id, Web::CSS::PreferredColorScheme) override;
    virtual void set_preferred_contrast(Compositing::PageId page_id, Web::CSS::PreferredContrast) override;
    virtual void set_preferred_motion(Compositing::PageId page_id, Web::CSS::PreferredMotion) override;
    virtual void set_preferred_languages(Compositing::PageId page_id, Vector<String>) override;
    virtual void set_browsing_behavior(Compositing::PageId page_id, WebView::BrowsingBehavior) override;
    virtual void set_enable_global_privacy_control(Compositing::PageId page_id, bool) override;
    virtual void set_geolocation_emulated_position(Compositing::PageId page_id, WebView::GeolocationPositionData, Optional<u16> error_code) override;
    virtual void geolocation_position_response(Compositing::PageId page_id, u64 request_id, WebView::GeolocationPositionData, Optional<u16> error_code) override;
    virtual void set_has_focus(Compositing::PageId page_id, bool) override;
    virtual void set_focused_navigable(Compositing::PageId page_id, Web::HTML::CrossProcessId navigable_id) override;
    virtual void consume_user_activation(Compositing::PageId page_id, Web::HTML::UserActivationConsumption) override;
    virtual void set_is_scripting_enabled(Compositing::PageId page_id, bool) override;
    virtual void set_zoom_level(Compositing::PageId page_id, double zoom_level) override;
    virtual void set_maximum_frames_per_second(Compositing::PageId page_id, double) override;
    virtual void set_window_position(Compositing::PageId page_id, Compositing::DevicePixelPoint) override;
    virtual void set_window_size(Compositing::PageId page_id, Compositing::DevicePixelSize) override;
    virtual void did_complete_window_rect_request(Compositing::PageId page_id, u64 completion_id) override;
    virtual void handle_file_return(Compositing::PageId page_id, i32 error, Optional<IPC::File> file, i32 request_id) override;
    virtual void blob_url_entry_removed(Utf16String url) override;
    virtual void did_delete_all_cookies(Compositing::PageId page_id, u64 request_id) override;
    virtual void update_visibility_state(Compositing::PageId page_id, Web::HTML::CrossProcessId navigable_id, Web::HTML::VisibilityState) override;
    virtual void reset_zoom(Compositing::PageId page_id) override;

    virtual void js_console_input(Compositing::PageId page_id, String) override;
    virtual void run_javascript(Compositing::PageId page_id, String) override;

    virtual void did_open_dialog_in_another_process(Compositing::PageId page_id, Web::Page::PendingDialog dialog, Utf16String message) override;
    virtual void alert_closed(Compositing::PageId page_id) override;
    virtual void confirm_closed(Compositing::PageId page_id, bool accepted) override;
    virtual void prompt_closed(Compositing::PageId page_id, Optional<Utf16String> response) override;
    virtual void color_picker_update(Compositing::PageId page_id, Optional<Color> picked_color, Web::HTML::ColorPickerUpdateState state) override;
    virtual void file_picker_closed(Compositing::PageId page_id, Vector<Web::HTML::SelectedFile> selected_files) override;
    virtual void select_dropdown_closed(Compositing::PageId page_id, Optional<u32> selected_item_id) override;

    virtual void retrieved_clipboard_entries(Compositing::PageId page_id, u64 request_id, Vector<Web::Clipboard::SystemClipboardItem>) override;

    virtual void toggle_media_play_state(Compositing::PageId page_id) override;
    virtual void toggle_media_mute_state(Compositing::PageId page_id) override;
    virtual void toggle_media_loop_state(Compositing::PageId page_id) override;
    virtual void toggle_media_fullscreen_state(Compositing::PageId page_id) override;
    virtual void toggle_media_controls_state(Compositing::PageId page_id) override;

    virtual void set_page_mute_state(Compositing::PageId page_id, Web::HTML::MuteState mute_state) override;

    virtual void set_user_style(Compositing::PageId page_id, String) override;

    virtual void take_document_screenshot(Compositing::PageId page_id) override;
    virtual void take_dom_node_screenshot(Compositing::PageId page_id, Compositing::UniqueNodeID node_id) override;

    virtual void request_internal_page_info(Compositing::PageId page_id, WebView::PageInfoType) override;

    virtual void get_selected_text(Compositing::PageId page_id, u64 request_id) override;
    virtual void get_selected_text_for_lookup(Compositing::PageId page_id, u64 request_id) override;
    virtual void select_word_for_dictionary_lookup(Compositing::PageId page_id, u64 request_id, Compositing::DevicePixelPoint position) override;
    virtual void cut_selected_text(Compositing::PageId page_id, u64 request_id) override;
    virtual void select_all(Compositing::PageId page_id) override;
    virtual void undo(Compositing::PageId page_id) override;
    virtual void redo(Compositing::PageId page_id) override;

    virtual void find_in_page(Compositing::PageId page_id, Utf16String query, CaseSensitivity) override;
    virtual void find_in_page_next_match(Compositing::PageId page_id) override;
    virtual void find_in_page_previous_match(Compositing::PageId page_id) override;

    virtual void paste(Compositing::PageId page_id, Utf16String text) override;
    virtual void paste_from_clipboard(Compositing::PageId page_id) override;
    virtual void set_marked_text_from_input_method(Compositing::PageId page_id, Utf16String text) override;
    virtual void commit_text_from_input_method(Compositing::PageId page_id, Utf16String text, i32 replacement_start, i32 replacement_length) override;
    virtual void unmark_text_from_input_method(Compositing::PageId page_id) override;

    virtual void system_time_zone_changed() override;
    virtual void set_system_font_family(String family) override;

    virtual void set_document_cookie_version_buffer(Compositing::PageId page_id, Core::AnonymousBuffer document_cookie_version_buffer) override;
    virtual void set_document_cookie_version_index(Compositing::PageId page_id, i64 document_id, Core::SharedVersionIndex document_index) override;
    virtual void cookies_changed(Compositing::PageId page_id, Vector<HTTP::Cookie::Cookie>) override;
    virtual void broadcast_channel_message(Web::HTML::BroadcastChannelMessage message) override;
    virtual void did_worker_agent_fail_loading_script(Web::HTML::WorkerAgentOwnerToken owner_token) override;
    virtual void did_worker_agent_report_exception(Web::HTML::WorkerAgentOwnerToken owner_token, Utf16String message, Utf16String filename, u32 lineno, u32 colno) override;
    virtual void did_worker_agent_close(Web::HTML::WorkerAgentOwnerToken owner_token) override;
    virtual void did_worker_agent_die(Web::HTML::WorkerAgentOwnerToken owner_token) override;

    virtual void request_close(Compositing::PageId page_id) override;
    virtual void force_close(Compositing::PageId page_id) override;

    virtual void exit_fullscreen(Compositing::PageId page_id) override;

    RefPtr<TestConnection> m_test_connection;
    RefPtr<WebView::CompositorConnection> m_compositor_connection;
    NonnullOwnPtr<PageHost> m_page_host;
    OwnPtr<DevToolsDebugger> m_devtools_debugger;

    HashMap<int, Web::FileRequest> m_requested_files {};
    int last_id { 0 };

    void enqueue_input_event(Web::QueuedInputEvent);
    void enqueue_mouse_event(Compositing::PageId page_id, Optional<Web::HTML::CrossProcessId> navigable_id, Compositing::MouseEvent);

    Queue<Web::QueuedInputEvent> m_input_event_queue;
    Gfx::SharedFontProvider* m_font_provider { nullptr };
    bool m_enable_test_mode { false };
};

}
