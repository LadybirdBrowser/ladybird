/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/Function.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/LexicalPath.h>
#include <AK/OwnPtr.h>
#include <AK/Queue.h>
#include <AK/String.h>
#include <AK/Types.h>
#include <AK/Utf16String.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/Forward.h>
#include <LibCore/Promise.h>
#include <LibCore/SharedVersion.h>
#include <LibDevTools/DevToolsDelegate.h>
#include <LibGfx/Color.h>
#include <LibGfx/Cursor.h>
#include <LibGfx/Forward.h>
#include <LibGfx/SharedImage.h>
#include <LibGfx/SharedImageBuffer.h>
#include <LibHTTP/Header.h>
#include <LibRequests/Forward.h>
#include <LibRequests/NetworkError.h>
#include <LibWeb/Bindings/Navigation.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/ActivateTab.h>
#include <LibWeb/HTML/AudioPlayState.h>
#include <LibWeb/HTML/ColorPickerUpdateState.h>
#include <LibWeb/HTML/FileFilter.h>
#include <LibWeb/HTML/Scripting/ScriptRegistry.h>
#include <LibWeb/HTML/SelectItem.h>
#include <LibWeb/Page/EventResult.h>
#include <LibWeb/Page/InputEvent.h>
#include <LibWeb/Page/ViewportIsFullscreen.h>
#include <LibWeb/WebDriver/Response.h>
#include <LibWebView/BookmarkStore.h>
#include <LibWebView/DOMNodeProperties.h>
#include <LibWebView/Forward.h>
#include <LibWebView/PageInfo.h>
#include <LibWebView/SessionHistory.h>
#include <LibWebView/Settings.h>
#include <LibWebView/StorageSetResult.h>
#include <LibWebView/WebContentClient.h>

namespace WebView {

class WEBVIEW_API ViewImplementation
    : public SettingsObserver
    , public BookmarkStoreObserver {
    friend class WebContentClient;

public:
    virtual ~ViewImplementation();

    static void for_each_view(Function<IterationDecision(ViewImplementation&)>);
    static Optional<ViewImplementation&> find_view_by_id(u64);

    u64 view_id() const { return m_view_id; }

    void set_url(Badge<WebContentClient>, URL::URL url) { set_url(move(url)); }
    URL::URL const& url() const { return m_url; }

    void set_title(Badge<WebContentClient>, Utf16String title) { m_title = move(title); }
    Utf16String const& title() const { return m_title; }

    void set_favicon(Badge<WebContentClient>, Gfx::Bitmap const&);
    Optional<String> const& favicon_base64_png() const { return m_favicon_base64_png; }

    String const& handle() const { return m_client_state.client_handle; }

    void create_new_process_for_cross_site_navigation(URL::URL const&, Variant<Empty, String, Web::HTML::POSTResource>, Web::Bindings::NavigationHistoryBehavior);

    void server_did_paint(Badge<WebContentClient>, i32 bitmap_id, Gfx::IntSize size);

    void set_window_position(Gfx::IntPoint);
    void set_window_size(Gfx::IntSize);
    void did_update_window_rect();

    void set_system_visibility_state(Web::HTML::VisibilityState);

    void load(URL::URL const&, Web::Bindings::NavigationHistoryBehavior = Web::Bindings::NavigationHistoryBehavior::Auto);
    void load_html(StringView);
    void load_navigation_error_page(StringView);

    void reload();
    enum class HistoryTraversalStatus : u8 {
        Started,
        NoEntry,
        Canceled,
    };
    // NB: The HTML Standard spells this algorithm argument "checkForCancelation".
    enum class CheckForCancelation : u8 {
        Yes,
        No,
        IfWebContentCannotTraverseTarget,
    };
    struct HistoryTraversalOutcome {
        HistoryTraversalStatus status { HistoryTraversalStatus::NoEntry };
        bool will_replace_web_content_process { false };
        bool will_change_top_level_entry { false };
        bool waiting_for_cancelation_check { false };
    };
    struct SessionHistoryTraversalMenuItem {
        int delta { 0 };
        String title;
        String url;
        Optional<String> favicon_base64_png;
    };
    [[nodiscard]] HistoryTraversalOutcome traverse_the_history_by_delta(
        int delta,
        CheckForCancelation = CheckForCancelation::Yes,
        Function<void(HistoryTraversalOutcome)> = nullptr);
    [[nodiscard]] Vector<SessionHistoryTraversalMenuItem> session_history_traversal_menu_items(int direction) const;

    void zoom_in();
    void zoom_out();
    void set_zoom(double zoom_level);
    void reset_zoom();
    double zoom_level() const { return m_zoom_level; }
    double device_pixel_ratio() const { return m_device_pixel_ratio; }
    Optional<u64> display_id() const { return m_display_id; }
    double maximum_frames_per_second() const { return m_maximum_frames_per_second; }

    void enqueue_input_event(Web::InputEvent);
    void did_finish_handling_input_event(Badge<WebContentClient>, Web::EventResult event_result);

    void set_preferred_color_scheme(Web::CSS::PreferredColorScheme);
    void set_preferred_contrast(Web::CSS::PreferredContrast);
    void set_preferred_motion(Web::CSS::PreferredMotion);

    void notify_cookies_changed(HashTable<String> const& changed_domains, ReadonlySpan<HTTP::Cookie::Cookie> page_cookies, ReadonlySpan<HTTP::Cookie::Cookie> host_cookies);
    void listen_for_host_cookie_changes(DevTools::DevToolsDelegate::OnHostCookieChange);
    void stop_listening_for_host_cookie_changes();
    void notify_indexed_database_changed(JsonObject);
    u64 add_indexed_database_change_listener(DevTools::DevToolsDelegate::OnIndexedDatabaseChange);
    void remove_indexed_database_change_listener(u64 listener_id);
    ErrorOr<Core::SharedVersionIndex> ensure_document_cookie_version_index(Badge<WebContentClient>, String const&);
    Optional<Core::SharedVersion> document_cookie_version(URL::URL const&) const;

    void notify_storage_changed(DevTools::DevToolsDelegate::StorageChange);
    u64 add_storage_change_listener(DevTools::DevToolsDelegate::OnStorageChange);
    void remove_storage_change_listener(u64 listener_id);

    void inspect_indexed_database_storage(DevTools::DevToolsDelegate::OnIndexedDBInspectionComplete);
    void inspect_indexed_database_objects(String const& host, Optional<JsonArray> names, JsonObject options, DevTools::DevToolsDelegate::OnIndexedDBInspectionComplete);
    void delete_indexed_database(String const& host, String const& name, DevTools::DevToolsDelegate::OnIndexedDBInspectionComplete);
    void clear_indexed_database_object_store(String const& host, String const& name, DevTools::DevToolsDelegate::OnIndexedDBInspectionComplete);
    void delete_indexed_database_record(String const& host, String const& name, DevTools::DevToolsDelegate::OnIndexedDBInspectionComplete);

    ByteString selected_text();
    ByteString cut_selected_text();
    Optional<String> selected_text_with_whitespace_collapsed();
    void select_all();
    void find_in_page(String const& query, CaseSensitivity = CaseSensitivity::CaseInsensitive);
    void find_in_page_next_match();
    void find_in_page_previous_match();

    void get_source();

    void inspect_dom_tree();
    void inspect_storage(Web::StorageAPI::StorageEndpointType, u64 request_id);
    Optional<StorageSetResult> set_session_storage_item(String const& key, String const& value);
    Optional<String> remove_session_storage_item(String const& key);
    bool clear_session_storage();
    void inspect_accessibility_tree();
    void get_hovered_node_id();
    void start_node_picker(DevTools::DevToolsDelegate::OnNodePickerEvent);
    void stop_node_picker();
    void clear_node_picker();
    bool is_node_picker_active() const { return m_node_picker_active; }
    void node_picker_hover(Web::DevicePixelPoint);
    void node_picker_pick(Web::DevicePixelPoint);
    void node_picker_preview(Web::DevicePixelPoint);
    void node_picker_cancel();

    void inspect_dom_node(Web::UniqueNodeID node_id, DOMNodeProperties::Type, Optional<Web::CSS::PseudoElement> pseudo_element, JsonValue options = {});
    void inspect_grid_layouts(Web::UniqueNodeID root_node_id);
    void inspect_current_grid(Web::UniqueNodeID node_id);
    void inspect_current_flexbox(Web::UniqueNodeID node_id, bool only_look_at_parents);
    void retrieve_devtools_sources(DevTools::DevToolsDelegate::OnSourcesReceived);
    void request_devtools_source(Web::HTML::ScriptRegistry::Identifier const&);
    void resolve_dom_node_url(Optional<Web::UniqueNodeID> node_id, String const& url, DevTools::DevToolsDelegate::OnResolvedURLReceived);
    void clear_inspected_dom_node();

    void highlight_dom_node(Web::UniqueNodeID node_id, Optional<Web::CSS::PseudoElement> pseudo_element);
    void clear_highlighted_dom_node();
    void highlight_flexbox(Web::UniqueNodeID node_id, JsonValue options);
    void clear_flexbox_highlight(Web::UniqueNodeID node_id);
    void highlight_grid(Web::UniqueNodeID node_id, JsonValue options);
    void clear_grid_highlight(Web::UniqueNodeID node_id);

    void set_listen_for_dom_mutations(bool);
    void did_connect_devtools_client();
    void did_disconnect_devtools_client();
    void get_dom_node_inner_html(Web::UniqueNodeID node_id);
    void get_dom_node_outer_html(Web::UniqueNodeID node_id);
    void set_dom_node_outer_html(Web::UniqueNodeID node_id, String const& html);
    void set_dom_node_text(Web::UniqueNodeID node_id, String const& text);
    void set_dom_node_tag(Web::UniqueNodeID node_id, String const& name);
    void add_dom_node_attributes(Web::UniqueNodeID node_id, ReadonlySpan<Attribute> attributes);
    void replace_dom_node_attribute(Web::UniqueNodeID node_id, String const& name, ReadonlySpan<Attribute> replacement_attributes);
    void create_child_element(Web::UniqueNodeID node_id);
    void create_child_text_node(Web::UniqueNodeID node_id);
    void insert_dom_node_before(Web::UniqueNodeID node_id, Web::UniqueNodeID parent_node_id, Optional<Web::UniqueNodeID> sibling_node_id);
    void clone_dom_node(Web::UniqueNodeID node_id);
    void remove_dom_node(Web::UniqueNodeID node_id);

    void list_style_sheets();
    void request_style_sheet_source(Web::CSS::StyleSheetIdentifier const&);

    void debug_request(ByteString const& request, ByteString const& argument = {});
    void set_content_blockers(Core::AnonymousBuffer const& patterns);

    void run_javascript(String const&);
    void js_console_input(String const&);
    void exit_fullscreen();

    void set_is_fullscreen(Web::ViewportIsFullscreen is_fullscreen);
    Web::ViewportIsFullscreen is_fullscreen() const { return m_is_fullscreen; }

    void alert_closed();
    void confirm_closed(bool accepted);
    void prompt_closed(Optional<String> const& response);
    void color_picker_update(Optional<Color> picked_color, Web::HTML::ColorPickerUpdateState state);
    void file_picker_closed(Vector<Web::HTML::SelectedFile> selected_files);
    void select_dropdown_closed(Optional<u32> const& selected_item_id);

    void paste_text_from_clipboard();
    void retrieved_clipboard_entries(u64 request_id, ReadonlySpan<Web::Clipboard::SystemClipboardItem>);

    // Used by platform input methods to drive marked/preedit-text composition, and to query the on-screen caret
    // position for placing IME overlays.
    void set_marked_text_from_input_method(Utf16String const& text);
    void commit_text_from_input_method(Utf16String const& text);
    void unmark_text_from_input_method();
    Optional<Web::DevicePixelRect> get_input_caret_rect();
    void set_input_caret_rect(Badge<WebContentClient>, Optional<Web::DevicePixelRect>);

    Web::HTML::MuteState page_mute_state() const { return m_mute_state; }
    void toggle_page_mute_state();

    void did_change_audio_play_state(Badge<WebContentClient>, Web::HTML::AudioPlayState);
    Web::HTML::AudioPlayState audio_play_state() const { return m_audio_play_state; }

    void did_update_navigation_buttons_state(Badge<WebContentClient>, bool back_enabled, bool forward_enabled);
    void did_update_session_history(Badge<WebContentClient>, Vector<Web::HTML::SessionHistoryEntryDescriptor>, Vector<i32>, size_t current_used_step_index);
    void did_update_session_history_for_testing(Badge<WebContentClient>, Vector<Web::HTML::SessionHistoryEntryDescriptor>, Vector<i32>, size_t current_used_step_index);
    void did_set_top_level_session_history(Badge<WebContentClient>, bool accepted, Vector<Web::HTML::SessionHistoryEntryDescriptor>, Vector<i32> used_steps, size_t current_used_step_index);
    void did_traverse_the_history_to_step(Badge<WebContentClient>, i32 step, bool step_was_available, Web::HTML::HistoryStepResult);
    void did_check_if_traverse_history_step_is_canceled(
        Badge<WebContentClient>, u64 request_id, i32 step, bool canceled);
    void did_reset_session_history_for_testing(Badge<WebContentClient>);
    void mark_web_content_session_history_stale_for_testing(Badge<WebContentClient>);
    void did_start_webdriver_navigation(Badge<WebContentClient>, URL::URL const&);
    String ui_process_session_history_for_testing(Badge<WebContentClient>) const;
    JsonValue webdriver_session_history() const;
    void wait_for_webdriver_navigation_completion(Badge<WebContentClient>, Optional<u64> page_load_timeout, Function<void(Web::WebDriver::Response)>);
    void did_change_needs_beforeunload_check(Badge<WebContentClient>, bool needs_beforeunload_check);
    void did_change_background_color(Badge<WebContentClient>, Gfx::Color);
    Gfx::Color page_background_color() const { return m_page_background_color; }

    void did_allocate_backing_stores(Badge<WebContentClient>, i32 front_bitmap_id, Gfx::SharedImage front_backing_store, i32 back_bitmap_id, Gfx::SharedImage back_backing_store);

    enum class ScreenshotType {
        Visible,
        Full,
    };
    NonnullRefPtr<Core::Promise<LexicalPath>> take_screenshot(ScreenshotType);
    NonnullRefPtr<Core::Promise<LexicalPath>> take_dom_node_screenshot(Web::UniqueNodeID);
    virtual void did_receive_screenshot(Badge<WebContentClient>, Gfx::ShareableBitmap const&);

    NonnullRefPtr<Core::Promise<String>> request_internal_page_info(PageInfoType);
    void did_receive_internal_page_info(Badge<WebContentClient>, PageInfoType, Optional<Core::AnonymousBuffer> const&);

    ErrorOr<LexicalPath> dump_gc_graph();

    void set_user_style_sheet(String const& source);

    void request_close();
    Function<void()> prepare_for_immediate_close();
    bool needs_beforeunload_check() const { return m_needs_beforeunload_check; }

    Function<void()> on_ready_to_paint;
    Function<String(Web::HTML::ActivateTab, Web::HTML::WebViewHints, Optional<u64>)> on_new_web_view;
    Function<void()> on_activate_tab;
    Function<void()> on_close;
    Function<void(URL::URL const&)> on_link_hover;
    Function<void()> on_link_unhover;
    Function<void(Utf16String const&)> on_title_change;
    Function<void(URL::URL const&)> on_url_change;
    Function<void(URL::URL const&, bool)> on_load_start;
    Function<void(URL::URL const&)> on_load_finish;

    struct NavigationListener {
        Function<void(URL::URL const&, bool)> on_load_start;
        Function<void(URL::URL const&)> on_load_finish;
    };
    u64 add_navigation_listener(NavigationListener);
    void remove_navigation_listener(u64 listener_id);

    Function<void(ByteString const& path, i32)> on_request_file;
    Function<void(Gfx::Bitmap const&)> on_favicon_change;
    Function<void(Gfx::Cursor const&)> on_cursor_change;
    Function<void(Gfx::IntPoint, ByteString const&)> on_request_tooltip_override;
    Function<void()> on_stop_tooltip_override;
    Function<void(ByteString const&)> on_enter_tooltip_area;
    Function<void()> on_leave_tooltip_area;
    Function<void(String const& message)> on_request_alert;
    Function<void(String const& message)> on_request_confirm;
    Function<void(String const& message, String const& default_)> on_request_prompt;
    Function<void(String const& message)> on_request_set_prompt_text;
    Function<void()> on_request_accept_dialog;
    Function<void()> on_request_dismiss_dialog;
    Function<void(JsonObject)> on_received_dom_tree;
    Function<void(DOMNodeProperties)> on_received_dom_node_properties;
    HashMap<u64, Function<void(ErrorOr<Vector<DevTools::DevToolsDelegate::StorageItem>>)>> on_received_storage_items;
    Function<void(JsonArray)> on_received_grid_layouts;
    Function<void(Optional<JsonObject>)> on_received_current_grid;
    Function<void(Optional<JsonObject>)> on_received_current_flexbox;
    Function<void(JsonObject)> on_received_accessibility_tree;
    Function<void(Web::UniqueNodeID)> on_received_hovered_node_id;
    Function<void(Mutation)> on_dom_mutation_received;
    Function<void(Optional<Web::UniqueNodeID> const& node_id)> on_finished_editing_dom_node;
    Function<void(String)> on_received_dom_node_html;
    Function<void(Vector<Web::CSS::StyleSheetIdentifier>)> on_received_style_sheet_list;
    Function<void(Web::CSS::StyleSheetIdentifier const&, URL::URL const&, String const&)> on_received_style_sheet_source;
    HashMap<u64, DevTools::DevToolsDelegate::OnSourcesReceived> on_received_devtools_sources;
    HashMap<Web::HTML::ScriptRegistry::Identifier, Function<void(Optional<Web::HTML::ScriptRegistry::Content>)>> on_received_devtools_source;
    HashMap<u64, DevTools::DevToolsDelegate::OnResolvedURLReceived> on_resolved_dom_node_url;
    Function<void(Web::HTML::ScriptRegistry::Description)> on_devtools_source_available;
    Function<void(JsonValue)> on_received_js_console_result;
    Function<void(ConsoleOutput)> on_console_message;
    Function<void(u64 request_id, URL::URL const&, ByteString const&, Vector<HTTP::Header> const&, ByteBuffer, Optional<String>)> on_network_request_started;
    Function<void(u64 request_id, u32 status_code, Optional<String> const&, Vector<HTTP::Header> const&)> on_network_response_headers_received;
    Function<void(u64 request_id, ByteBuffer)> on_network_response_body_received;
    Function<void(u64 request_id, u64 body_size, Requests::RequestTimingInfo const&, Optional<Requests::NetworkError> const&)> on_network_request_finished;
    Function<void(i32 count_waiting)> on_resource_status_change;
    Function<void()> on_restore_window;
    Function<void(Gfx::IntPoint)> on_reposition_window;
    Function<void(Gfx::IntSize)> on_resize_window;
    Function<void()> on_maximize_window;
    Function<void()> on_minimize_window;
    Function<void()> on_fullscreen_window;
    Function<void()> on_exit_fullscreen_window;
    Function<void(Color current_color)> on_request_color_picker;
    Function<void(Web::HTML::FileFilter const& accepted_file_types, Web::HTML::AllowMultipleFiles)> on_request_file_picker;
    Function<void(Gfx::IntPoint content_position, i32 minimum_width, Vector<Web::HTML::SelectItem> items)> on_request_select_dropdown;
    Function<void(Web::KeyEvent const&)> on_finish_handling_key_event;
    Function<void(Web::DragEvent const&)> on_finish_handling_drag_event;
    Function<void(String const&)> on_test_finish;
    Function<void(double milliseconds)> on_set_test_timeout;
    Function<void(JsonValue)> on_reference_test_metadata;
    Function<void(size_t current_match_index, Optional<size_t> const& total_match_count)> on_find_in_page;
    Function<void(Gfx::Color)> on_theme_color_change;
    Function<void(Gfx::Color)> on_page_background_color_change;
    Function<void(Web::HTML::AudioPlayState)> on_audio_play_state_changed;
    Function<void()> on_web_content_crashed;
    Function<void()> on_web_content_process_change_for_cross_site_navigation;

    Menu& page_context_menu() { return *m_page_context_menu; }
    Menu& link_context_menu() { return *m_link_context_menu; }
    Menu& image_context_menu() { return *m_image_context_menu; }
    Menu& media_context_menu() { return *m_media_context_menu; }

    void did_request_page_context_menu(Badge<WebContentClient>, Gfx::IntPoint content_position, Web::ContextMenuForInputEventsTarget for_input_events_target);
    void did_request_link_context_menu(Badge<WebContentClient>, Gfx::IntPoint content_position, URL::URL url);
    void did_request_image_context_menu(Badge<WebContentClient>, Gfx::IntPoint content_position, URL::URL url, Optional<Gfx::ShareableBitmap> bitmap);
    void did_request_media_context_menu(Badge<WebContentClient>, Gfx::IntPoint content_position, Web::Page::MediaContextMenu menu);

    Action& navigate_back_action() { return *m_navigate_back_action; }
    Action& navigate_forward_action() { return *m_navigate_forward_action; }
    Action& toggle_bookmark_action() { return *m_toggle_bookmark_action; }
    Action& reset_zoom_action() { return *m_reset_zoom_action; }

    WebContentClient& client();
    WebContentClient const& client() const;

    virtual Web::DevicePixelSize viewport_size() const = 0;
    virtual Gfx::IntPoint to_content_position(Gfx::IntPoint widget_position) const = 0;
    virtual Gfx::IntPoint to_widget_position(Gfx::IntPoint content_position) const = 0;

protected:
    static constexpr auto ZOOM_MIN_LEVEL = 0.3;
    static constexpr auto ZOOM_MAX_LEVEL = 5.0;
    static constexpr auto ZOOM_STEP = 0.1;

    ViewImplementation();

    u64 page_id() const;

    void set_url(URL::URL);
    void did_start_navigation(URL::URL const&, Variant<Empty, String, Web::HTML::POSTResource>, bool is_redirect, Web::Bindings::NavigationHistoryBehavior);
    void did_cancel_navigation(URL::URL const&);
    void did_finish_navigation(URL::URL const&);
    void complete_webdriver_navigation_completion(u64 request_id, Web::WebDriver::Response);
    void complete_webdriver_pending_navigation_if_url_matches(URL::URL const&);
    void update_navigation_action_state();
    TraversableSessionHistory::UpdateResult update_session_history_from_web_content(Vector<Web::HTML::SessionHistoryEntryDescriptor>, Vector<i32> used_steps, size_t current_used_step_index, bool pending_step_after_fallback_load_was_restored, bool seed_web_content_on_invalid_snapshot);
    bool adopt_web_content_session_history_after_rejected_seed(Vector<Web::HTML::SessionHistoryEntryDescriptor>, Vector<i32> used_steps, size_t current_used_step_index);
    enum class SessionHistoryDumpMode {
        IfDebuggingEnabled,
        Always,
    };
    void dump_session_history(StringView reason, SessionHistoryDumpMode = SessionHistoryDumpMode::IfDebuggingEnabled) const;
    bool restore_pending_session_history_navigation(StringView reason);
    void abandon_pending_web_content_session_history_seed();
    enum class AllowCurrentEntryReconstruction : u8 {
        No,
        Yes,
    };
    void seed_web_content_session_history_from_ui_process(AllowCurrentEntryReconstruction = AllowCurrentEntryReconstruction::No);
    void prepare_to_seed_web_content_session_history_from_ui_process();
    void restore_current_session_history_entry_from_ui_process();
    void load_current_session_history_entry_from_ui_process();
    void load_session_history_traversal_target_from_ui_process(TraversableSessionHistory::TraversalTarget const&, StringView dump_reason);
    NonnullRefPtr<Core::Promise<Empty>> reset_session_history_for_testing();

    virtual void update_zoom();
    virtual bool should_manage_session_history_in_ui_process() const { return true; }
    String current_host() const;
    void apply_zoom_for_current_host();

    void handle_resize();
    void set_page_background_color_to_system_canvas(bool dark);
    void set_page_background_color(Gfx::Color);
    Gfx::Color preferred_canvas_background_color() const;
    void load_crash_page_html(StringView, URL::URL const& crashed_url);

    enum class CreateNewClient {
        No,
        Yes,
    };
    virtual void initialize_client(CreateNewClient = CreateNewClient::Yes);

    enum class LoadErrorPage {
        No,
        Yes,
    };
    void handle_web_content_process_crash(LoadErrorPage = LoadErrorPage::Yes);

    virtual void default_zoom_level_factor_changed() override;
    virtual void zoom_per_host_changed(StringView host) override;
    virtual void languages_changed() override;
    virtual void browsing_behavior_changed() override;
    virtual void autoplay_settings_changed() override;
    virtual void global_privacy_control_changed() override;

    virtual void bookmarks_changed() override;
    void update_bookmark_action();

    void initialize_context_menus();

    struct SharedBitmap {
        i32 id { -1 };
        Web::DevicePixelSize last_painted_size;
        OwnPtr<Gfx::SharedImageBuffer> shared_image_buffer;
    };

    struct ClientState {
        RefPtr<WebContentClient> client;
        String client_handle;
        SharedBitmap front_bitmap;
        SharedBitmap back_bitmap;
        u64 page_index { 0 };
        bool has_usable_bitmap { false };
    } m_client_state;

    URL::URL m_url;
    Utf16String m_title;
    Optional<String> m_favicon_base64_png;
    bool m_is_showing_crash_page { false };

    double m_zoom_level { 1.0 };
    double m_device_pixel_ratio { 1.0 };
    Optional<u64> m_display_id;
    double m_maximum_frames_per_second { 60.0 };

    RefPtr<Menu> m_page_context_menu;
    RefPtr<Menu> m_link_context_menu;
    RefPtr<Menu> m_image_context_menu;
    RefPtr<Menu> m_media_context_menu;

    RefPtr<Action> m_navigate_back_action;
    RefPtr<Action> m_navigate_forward_action;

    RefPtr<Action> m_toggle_bookmark_action;

    RefPtr<Action> m_reset_zoom_action;

    RefPtr<Action> m_search_selected_text_action;
    Optional<String> m_search_text;

    RefPtr<Action> m_take_visible_screenshot_action;
    RefPtr<Action> m_take_full_screenshot_action;

    RefPtr<Action> m_open_in_new_tab_action;
    RefPtr<Action> m_open_in_new_window_action;
    RefPtr<Action> m_copy_url_action;
    URL::URL m_context_menu_url;

    RefPtr<Action> m_open_image_action;
    RefPtr<Action> m_save_image_action;
    RefPtr<Action> m_copy_image_action;
    Optional<Gfx::ShareableBitmap> m_image_context_menu_bitmap;

    RefPtr<Action> m_open_audio_action;
    RefPtr<Action> m_open_video_action;
    RefPtr<Action> m_media_play_action;
    RefPtr<Action> m_media_pause_action;
    RefPtr<Action> m_media_mute_action;
    RefPtr<Action> m_media_unmute_action;
    RefPtr<Action> m_media_show_controls_action;
    RefPtr<Action> m_media_hide_controls_action;
    RefPtr<Action> m_media_loop_action;
    RefPtr<Action> m_media_enter_fullscreen_action;
    RefPtr<Action> m_media_exit_fullscreen_action;

    Queue<Web::InputEvent> m_pending_input_events;

    RefPtr<Core::Timer> m_backing_store_shrink_timer;

    OwnPtr<Gfx::SharedImageBuffer> m_backup_shared_image_buffer;
    Web::DevicePixelSize m_backup_bitmap_size;
    Gfx::Color m_page_background_color { 255, 255, 255 };
    Gfx::Color m_system_canvas_background_color { 255, 255, 255 };
    Web::CSS::PreferredColorScheme m_preferred_color_scheme { Web::CSS::PreferredColorScheme::Auto };

    bool m_should_suppress_history_for_current_load { false };
    bool m_should_suppress_history_for_next_load { false };
    size_t m_crash_count = 0;
    RefPtr<Core::Timer> m_repeated_crash_timer;

    RefPtr<Core::Promise<LexicalPath>> m_pending_screenshot;
    RefPtr<Core::Promise<String>> m_pending_info_request;

    Web::HTML::VisibilityState m_system_visibility_state { Web::HTML::VisibilityState::Hidden };

    Web::HTML::AudioPlayState m_audio_play_state { Web::HTML::AudioPlayState::Paused };
    size_t m_number_of_elements_playing_audio { 0 };

    Web::HTML::MuteState m_mute_state { Web::HTML::MuteState::Unmuted };

    struct PendingSessionHistoryNavigation {
        enum class WebContentRestoreMode : u8 {
            PreserveCurrentProcessState,
            RestoreFromUIProcess,
        };

        URL::URL url;
        TraversableSessionHistory previous_session_history;
        WebContentRestoreMode web_content_restore_mode { WebContentRestoreMode::PreserveCurrentProcessState };
    };
    static StringView pending_session_history_navigation_web_content_restore_mode_to_string(PendingSessionHistoryNavigation::WebContentRestoreMode);

    struct PendingWebContentSessionHistorySeed {
        bool should_send_entries { false };
        bool ignore_updates_until_seed { false };
        bool waiting_for_ack { false };
        bool should_reseed_after_current_history_load { false };
        Optional<i32> step_after_loading_top_level_entry;

        void clear() { *this = {}; }
    };

    struct PendingSessionHistoryTraversal {
        enum class Stage : u8 {
            ApplyingInWebContent,
            CheckingCancelation,
            LoadingEntryFromUIProcess,
            ReplacingWebContentProcess,
            RestoringNestedStepAfterSeed,
        };

        i32 target_step { 0 };
        size_t target_step_index { 0 };
        u64 cancelation_check_request_id { 0 };
        bool will_change_top_level_entry { false };
        bool will_replace_web_content_process { false };
        Stage stage { Stage::ApplyingInWebContent };
        Function<void(HistoryTraversalOutcome)> on_cancelation_check_complete;
    };
    static StringView pending_session_history_traversal_stage_to_string(PendingSessionHistoryTraversal::Stage);

    TraversableSessionHistory m_session_history;
    bool m_current_web_content_session_history_matches_mirror { false };
    Optional<PendingSessionHistoryNavigation> m_pending_session_history_navigation;
    Optional<PendingSessionHistoryTraversal> m_pending_session_history_traversal;
    u64 m_next_traverse_history_step_cancelation_check_request_id { 0 };
    Optional<URL::URL> m_session_history_entry_url_loading_from_ui_process;
    PendingWebContentSessionHistorySeed m_pending_web_content_session_history_seed;
    Optional<URL::URL> m_webdriver_pending_navigation_url;
    bool m_webdriver_pending_navigation_completes_with_session_history_update { false };
    RefPtr<Core::Promise<Empty>> m_pending_session_history_reset_for_testing;

    struct WebDriverNavigationCompletionRequest {
        Function<void(Web::WebDriver::Response)> on_complete;
        RefPtr<Core::Timer> timer;
        u64 navigation_listener_id { 0 };
    };
    u64 m_next_webdriver_navigation_completion_request_id { 0 };
    HashMap<u64, OwnPtr<WebDriverNavigationCompletionRequest>> m_pending_webdriver_navigation_completion_requests;

    // Most recent caret position pushed by WebContent, Used for placing platform IME overlays without a sync IPC.
    Optional<Web::DevicePixelRect> m_input_caret_rect;

    Web::ViewportIsFullscreen m_is_fullscreen { Web::ViewportIsFullscreen::No };

    Core::AnonymousBuffer m_document_cookie_version_buffer;
    HashMap<String, Core::SharedVersionIndex> m_document_cookie_version_indices;
    DevTools::DevToolsDelegate::OnHostCookieChange m_on_host_cookie_change;
    HashMap<u64, DevTools::DevToolsDelegate::OnIndexedDatabaseChange> m_indexed_database_change_listeners;
    u64 m_next_indexed_database_change_listener_id { 1 };

    HashMap<u64, DevTools::DevToolsDelegate::OnStorageChange> m_storage_change_listeners;
    u64 m_next_storage_change_listener_id { 1 };
    u64 m_next_devtools_sources_request_id { 1 };
    u64 m_next_resolve_dom_node_url_request_id { 1 };

    HashMap<u64, DevTools::DevToolsDelegate::OnIndexedDBInspectionComplete> m_pending_indexed_database_inspection_requests;
    u64 m_next_indexed_database_inspection_request_id { 1 };

    // FIXME: Reconcile this ID with `page_id`. The latter is only unique per WebContent connection, whereas the view ID
    //        is required to be globally unique for Firefox DevTools.
    u64 m_view_id { 0 };

    HashMap<u64, NavigationListener> m_navigation_listeners;
    u64 m_next_navigation_listener_id { 1 };

    enum class NodePickerRequestType : u8 {
        Hovered,
        Picked,
        Previewed,
    };
    void request_node_picker_hit_test(NodePickerRequestType, Web::DevicePixelPoint);
    void did_receive_node_picker_hit_test(u64 request_id, Web::UniqueNodeID);
    void did_receive_indexed_database_inspection(u64 request_id, JsonObject);

    bool m_node_picker_active { false };
    Optional<Web::UniqueNodeID> m_node_picker_hovered_node_id;
    u64 m_next_node_picker_request_id { 1 };
    HashMap<u64, NodePickerRequestType> m_pending_node_picker_requests;
    DevTools::DevToolsDelegate::OnNodePickerEvent m_on_node_picker_event;

    bool m_devtools_connected { false };
    bool m_needs_beforeunload_check { true };
};

}
