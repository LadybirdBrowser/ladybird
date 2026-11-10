/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <AK/Queue.h>
#include <Hub/View.h>
#include <LibWebView/ContextMenuRequest.h>
#include <LibWebView/HistoryStore.h>
#include <LibWebView/WebCompatReport.h>
#include <UI/Qt/Actions.h>
#include <UI/Qt/BookmarkStore.h>
#include <UI/Qt/HubConnection.h>
#include <UI/Qt/SessionStore.h>

namespace Ladybird {

// The UI's side of a tab's view. The view itself lives in the Hub. This holds what the UI needs to show it, and offers
// the rest of the UI the calls and callbacks that it takes to use the view.
class RemoteView : public BookmarkStoreObserver {
public:
    using InputMethodState = WebView::ViewImplementation::InputMethodState;
    using NavigationListener = WebView::ViewImplementation::NavigationListener;
    using ScreenRects = WebView::ViewImplementation::ScreenRects;
    using ScreenshotType = WebView::ViewImplementation::ScreenshotType;
    struct SessionHistoryTraversalMenuItem {
        i32 step { 0 };
        String title;
        String url;
        Optional<ByteBuffer> favicon_png;
    };

    // Shows the view that the Hub has made, if any, or else makes a new one.
    RemoteView(Optional<u64> view_id, Hub::View::InitialState);
    virtual ~RemoteView();

    static RemoteView* find(u64 view_id);
    static void for_each(Function<void(RemoteView&)> const&);

    void did_request_context_menu(WebView::ContextMenuRequest const&);
    void did_make_link_to_highlight(Optional<URL::URL>);

    void set_state(Hub::ViewState state)
    {
        auto previous_state = exchange(m_state, move(state));
        update_view_actions();
        if (on_state_change)
            on_state_change(previous_state);
    }
    void set_initial_state(Hub::ViewState state) { set_state(move(state)); }

    void did_update_zoom() { update_zoom(); }
    void did_get_selected_text(u64 request_id, Optional<ByteString>);
    void did_finish_close_preflight(u64 request_id, bool may_close);
    void did_get_page_dumps(u64 request_id, Optional<String>);

    // The step of the session history that is this far from the current one, if there is one.
    Optional<i32> history_step_at_delta(int delta) const;

    WebView::WebCompatReport::Page describe_page() const;
    NonnullRefPtr<Core::Promise<String>> request_page_dumps();
    void did_start_navigation_load(URL::URL const&);
    void did_finish_navigation_load(URL::URL const&);

    void did_finish_handling_key_event(u64 input_id, Web::KeyEvent);
    void did_finish_handling_mouse_event(u64 input_id, Web::MouseEvent, Web::EventResult);
    void did_finish_handling_drag_event(u64 input_id, Web::DragEvent);
    void release_input(u64 input_id) { m_input_in_hub.remove(input_id); }

    WebView::IsPrivate is_private() const { return m_is_private; }
    URL::Origin permission_origin() const { return *m_state.permission_origin; }
    Vector<WebView::SitePermissionRow> site_permission_rows() const { return m_state.site_permission_rows; }
    void set_site_permission(URL::Origin const& origin, Web::Permissions::PermissionName name, Optional<WebView::PermissionDecision> decision) { HubConnection::the().async_view_set_site_permission(view_id(), origin, name, decision); }
    void reset_site_permissions(URL::Origin const& origin) { HubConnection::the().async_view_reset_site_permissions(view_id(), origin); }
    void did_show_permission_prompt(WebView::PermissionPromptRequest const& request) { HubConnection::the().async_view_did_show_permission_prompt(view_id(), request); }
    u64 view_id() const { return m_view_id; }
    pid_t web_content_pid() const { return m_state.web_content_pid; }
    URL::URL url() const { return m_state.url; }
    URL::URL const& displayed_url() const { return m_state.displayed_url; }
    Utf16String const& title() const { return m_state.title; }
    Optional<String> const& favicon_hash() const { return m_favicon_hash; }
    void did_change_favicon(Optional<Gfx::Bitmap const&>);
    void did_visit_page(URL::URL const&, Optional<String> const& title, WebView::HistoryVisitTransition);
    void did_change_visited_page_title(URL::URL const&, String const& title);
    void did_finish_loading(URL::URL const&);
    String const& handle() const { return m_state.handle; }
    void set_window_position(Gfx::IntPoint position) { HubConnection::the().async_view_set_window_position(view_id(), position); }
    void set_window_size(Gfx::IntSize size) { HubConnection::the().async_view_set_window_size(view_id(), size); }
    void set_system_visibility_state(Web::HTML::VisibilityState visibility_state) { HubConnection::the().async_view_set_system_visibility_state(view_id(), move(visibility_state)); }
    void set_presents_from_cpu_memory(bool presents_from_cpu_memory) { HubConnection::the().async_view_set_presents_from_cpu_memory(view_id(), presents_from_cpu_memory); }
    void set_has_system_focus(bool has_system_focus) { HubConnection::the().async_view_set_has_system_focus(view_id(), has_system_focus); }
    void load(URL::URL const& url, Web::Bindings::NavigationHistoryBehavior navigation_history_behavior = Web::Bindings::NavigationHistoryBehavior::Auto) { HubConnection::the().async_view_load(view_id(), url, navigation_history_behavior); }
    void load_from_user_input(URL::URL const&);
    void load_from_user_input(StringView input);
    void load_from_user_input(StringView input, Optional<URL::URL> fallback_url);
    void open_url_in_new_tab(URL::URL const&, Web::HTML::ActivateTab);
    void open_url_in_new_window(URL::URL const&, WebView::IsPrivate);
    void load(Web::HTML::PreparedNavigationDescriptor navigation) { HubConnection::the().async_view_load_navigation(view_id(), move(navigation)); }
    void open_navigation_in_new_tab(Web::HTML::PreparedNavigationDescriptor navigation, Web::HTML::ActivateTab activate_tab) { HubConnection::the().async_view_open_navigation_in_new_tab(view_id(), move(navigation), activate_tab); }
    void open_navigation_in_new_window(Web::HTML::PreparedNavigationDescriptor navigation, WebView::IsPrivate is_private) { HubConnection::the().async_view_open_navigation_in_new_window(view_id(), move(navigation), is_private); }
    void set_next_history_visit_transition(WebView::HistoryVisitTransition transition) { HubConnection::the().async_view_set_next_history_visit_transition(view_id(), move(transition)); }
    void load_html(StringView html) { HubConnection::the().async_view_load_html(view_id(), html); }
    void load_navigation_error_page(StringView input);
    void reload(WebView::BypassCache bypass_cache = WebView::BypassCache::No) { HubConnection::the().async_view_reload(view_id(), move(bypass_cache)); }
    void stop_loading() { HubConnection::the().async_view_stop_loading(view_id()); }
    bool is_loading() const { return m_state.is_loading; }
    bool crash_overlay_active() const { return m_state.crash_overlay_active; }
    String crash_overlay_failed_url() const { return m_state.crash_overlay_failed_url; }
    Optional<ByteString> crash_report_name() const { return m_state.crash_report_name; }
    Optional<String> crash_report_website() const { return m_state.crash_report_website; }
    void traverse_the_history_to_step(i32 step) { HubConnection::the().async_view_traverse_the_history_to_step(view_id(), step); }
    void traverse_the_history_by_delta(i32 delta) { HubConnection::the().async_view_traverse_the_history_by_delta(view_id(), delta); }
    bool can_navigate_back() const { return m_state.can_navigate_back; }
    bool can_navigate_forward() const { return m_state.can_navigate_forward; }
    enum class PromptForPath : u8 {
        No,
        Yes,
    };
    void download_url(URL::URL const& url, PromptForPath prompt_for_path, Optional<String> const& image_mime_type = {}) { HubConnection::the().async_view_download_url(view_id(), url, prompt_for_path == PromptForPath::Yes, image_mime_type); }
    Vector<WebView::HistoryEntry> recent_history_entries(size_t limit) const;
    Optional<SessionTabId> session_tab_id() const { return m_session_tab_id; }
    void set_session_tab_id(SessionTabId session_tab_id)
    {
        m_session_tab_id = session_tab_id;
        HubConnection::the().async_view_set_reports_session_history(view_id(), true);
    }
    void clear_session_tab_id()
    {
        m_session_tab_id = {};
        HubConnection::the().async_view_set_reports_session_history(view_id(), false);
    }
    void did_change_session_history(WebView::SessionHistorySnapshot, URL::URL);
    void zoom_in() { HubConnection::the().async_view_zoom_in(view_id()); }
    void zoom_out() { HubConnection::the().async_view_zoom_out(view_id()); }
    void reset_zoom() { HubConnection::the().async_view_reset_zoom(view_id()); }
    double zoom_level() const { return m_state.zoom_level; }
    double device_pixel_ratio() const { return m_device_pixel_ratio; }
    Optional<u64> display_id() const { return m_display_id; }
    void set_display_metadata(Optional<u64> display_id, double maximum_frames_per_second)
    {
        m_display_id = display_id;
        HubConnection::the().async_view_set_display_metadata(view_id(), display_id, maximum_frames_per_second);
    }
    void update_screen_rects();
    void set_scales_frames_to_fit(bool scales_frames_to_fit)
    {
        m_scales_frames_to_fit = scales_frames_to_fit;
        HubConnection::the().async_view_set_scales_frames_to_fit(view_id(), scales_frames_to_fit);
    }
    void enable_picture_in_picture_controls() { HubConnection::the().async_view_enable_picture_in_picture_controls(view_id()); }
    void enqueue_input_event(Web::InputEvent);
    String current_host_for_settings() const { return m_state.host_for_settings; }
    void update_interface_color_scheme() { HubConnection::the().async_view_update_interface_color_scheme(view_id()); }
    void update_system_theme(Core::AnonymousBuffer const& theme) { HubConnection::the().async_view_set_system_theme(view_id(), theme); }
    NonnullRefPtr<Core::Promise<ByteString>> selected_text();
    bool look_up_selected_text_at(Gfx::IntPoint widget_position);
    void find_in_page(Utf16String const& query, CaseSensitivity case_sensitivity = CaseSensitivity::CaseInsensitive, bool highlight_all_matches = false) { HubConnection::the().async_view_find_in_page(view_id(), query, move(case_sensitivity), highlight_all_matches); }
    void find_in_page_next_match() { HubConnection::the().async_view_find_in_page_next_match(view_id()); }
    void find_in_page_previous_match() { HubConnection::the().async_view_find_in_page_previous_match(view_id()); }
    void find_in_page_end() { HubConnection::the().async_view_find_in_page_end(view_id()); }
    bool can_report_website_issue() const { return m_state.can_report_website_issue; }
    bool can_undo() const { return m_state.can_undo; }
    bool can_redo() const { return m_state.can_redo; }
    bool can_view_source() const { return m_state.can_view_source; }
    void report_website_issue() { HubConnection::the().async_view_report_website_issue(view_id()); }
    void request_accessibility_tree() { HubConnection::the().async_view_request_accessibility_tree(view_id()); }
    void perform_accessibility_action(i64 node_id, String action) { HubConnection::the().async_view_perform_accessibility_action(view_id(), node_id, move(action)); }
    void clear_node_picker() { HubConnection::the().async_view_clear_node_picker(view_id()); }
    bool is_node_picker_active() const { return m_state.is_node_picker_active; }
    void node_picker_hover(Web::DevicePixelPoint device_pixel_point) { HubConnection::the().async_view_node_picker_hover(view_id(), move(device_pixel_point)); }
    void node_picker_pick(Web::DevicePixelPoint device_pixel_point) { HubConnection::the().async_view_node_picker_pick(view_id(), move(device_pixel_point)); }
    void node_picker_preview(Web::DevicePixelPoint device_pixel_point) { HubConnection::the().async_view_node_picker_preview(view_id(), move(device_pixel_point)); }
    void node_picker_cancel() { HubConnection::the().async_view_node_picker_cancel(view_id()); }
    WebView::ContentBlockingSnapshot content_blocking_snapshot() const { return m_state.content_blocking_snapshot; }
    void alert_closed() { HubConnection::the().async_view_alert_closed(view_id()); }
    void confirm_closed(bool accepted) { HubConnection::the().async_view_confirm_closed(view_id(), accepted); }
    void prompt_closed(Optional<Utf16String> const& response) { HubConnection::the().async_view_prompt_closed(view_id(), response); }
    void color_picker_update(Optional<Color> picked_color, Web::HTML::ColorPickerUpdateState state) { HubConnection::the().async_view_color_picker_update(view_id(), move(picked_color), move(state)); }
    void date_picker_closed(Optional<Utf16String> picked_value) { HubConnection::the().async_view_date_picker_closed(view_id(), move(picked_value)); }
    void file_picker_closed(Vector<Web::HTML::SelectedFile> selected_files) { HubConnection::the().async_view_file_picker_closed(view_id(), move(selected_files)); }
    void select_dropdown_closed(Optional<u32> const& selected_item_id) { HubConnection::the().async_view_select_dropdown_closed(view_id(), selected_item_id); }
    void set_marked_text_from_input_method(Utf16String const& text) { HubConnection::the().async_view_set_marked_text_from_input_method(view_id(), text); }
    void commit_text_from_input_method(Utf16String const& text, i32 replacement_start = 0, i32 replacement_length = 0) { HubConnection::the().async_view_commit_text_from_input_method(view_id(), text, replacement_start, replacement_length); }
    InputMethodState const& input_method_state() const { return m_state.input_method_state; }
    Web::HTML::MuteState page_mute_state() const { return m_state.page_mute_state; }
    void toggle_page_mute_state() { HubConnection::the().async_view_toggle_page_mute_state(view_id()); }
    bool tab_indicator_visible(WebView::TabIndicator indicator) const
    {
        switch (indicator) {
        case WebView::TabIndicator::PlayingAudio:
            return m_state.plays_audio_indicator_visible;
        case WebView::TabIndicator::UsingLocation:
            return m_state.uses_location_indicator_visible;
        case WebView::TabIndicator::UsingMicrophone:
            return m_state.uses_microphone_indicator_visible;
        }
        VERIFY_NOT_REACHED();
    }
    Gfx::Color page_background_color() const { return m_state.page_background_color; }
    WebView::PageBackingStores const& backing_stores() const { return m_backing_stores; }
    void take_and_save_screenshot(ScreenshotType screenshot_type) { HubConnection::the().async_view_take_and_save_screenshot(view_id(), move(screenshot_type)); }
    void save_screenshot(Gfx::Bitmap const& bitmap) { HubConnection::the().async_view_save_screenshot(view_id(), bitmap.to_shareable_bitmap()); }
    void request_close() { HubConnection::the().async_view_request_close(view_id()); }
    void request_close_preflight(Function<void(bool)> on_complete);
    bool needs_beforeunload_check() const;
    u64 add_navigation_listener(NavigationListener);
    void remove_navigation_listener(u64 listener_id) { m_navigation_listeners.remove(listener_id); }
    Action& navigate_back_action() { return *m_navigate_back_action; }
    Action& navigate_forward_action() { return *m_navigate_forward_action; }
    Action& toggle_bookmark_action() { return *m_toggle_bookmark_action; }
    void update_bookmark_action();
    Action& reset_zoom_action() { return *m_reset_zoom_action; }
    Action& copy_link_to_highlight_action() { return *m_copy_link_to_highlight_action; }
    Action& translate_action() { return *m_translate_action; }
    bool is_translated() const { return m_state.is_translated; }
    Menu& translation_menu() { return *m_translation_menu; }

    Vector<WebView::ViewImplementation::SessionHistoryTraversalItem> session_history_traversal_items(int direction) const;
    Vector<SessionHistoryTraversalMenuItem> session_history_traversal_menu_items(int direction) const;
    WebView::SessionHistorySnapshot session_history_snapshot() const;
    ErrorOr<void> restore_session_history_from_snapshot(WebView::SessionHistorySnapshot);
    void exit_fullscreen() { HubConnection::the().async_view_exit_fullscreen(view_id()); }
    void deny_fullscreen_window() { HubConnection::the().async_view_deny_fullscreen_window(view_id()); }
    void set_is_fullscreen(Web::ViewportIsFullscreen is_fullscreen) { HubConnection::the().async_view_set_is_fullscreen(view_id(), is_fullscreen); }
    Function<void()> prepare_for_close_without_prompting();
    void prepare_for_immediate_close();

    bool scales_frames_to_fit() const { return m_scales_frames_to_fit; }

    void did_install_backing_stores(u64 page_id, Vector<i32> bitmap_ids, Vector<Gfx::SharedImage> backing_stores);
    void did_add_backing_stores(Vector<i32> bitmap_ids, Vector<Gfx::SharedImage> backing_stores) { m_backing_stores.add(move(bitmap_ids), move(backing_stores)); }
    void did_retire_backing_stores(ReadonlySpan<i32> bitmap_ids) { m_backing_stores.retire(bitmap_ids); }
    void did_present_backing_store(u64 page_id, i32 bitmap_id, Gfx::IntSize, double device_pixel_ratio);
    void did_accept_backing_store(i32 bitmap_id, Gfx::IntRect damage_rect);
    void did_keep_front_backing_store_as_backup() { m_backing_stores.keep_front_as_backup(); }
    void did_clear_backup_backing_store() { m_backing_stores.clear_backup(); }
    void did_clear_backing_stores() { m_backing_stores.clear(); }

    // Hands a backing store back to the page that presented it, which can have outlived the view.
    static void release_backing_store_to_page(u64 page_id, i32 bitmap_id);

    // The page that presented the backing stores that the view holds.
    u64 backing_stores_page_id() const { return m_backing_stores_page_id; }

protected:
    virtual void show_context_menu(Menu&, Gfx::IntPoint) { }
    virtual void did_accept_presented_backing_store(i32, Gfx::IntRect) { }
    virtual void release_backing_store(u64 page_id, i32 bitmap_id) { release_backing_store_to_page(page_id, bitmap_id); }

    void set_page_background_color_to_system_canvas(bool dark) { HubConnection::the().async_view_set_page_background_color_to_system_canvas(view_id(), dark); }
    void set_device_pixel_ratio_and_viewport(double device_pixel_ratio, Web::DevicePixelSize);
    virtual Optional<ScreenRects> screen_rects() const = 0;
    virtual void update_zoom() { }
    static Gfx::IntRect rect_fitting_frame(Gfx::IntSize frame_size, Gfx::IntSize target_size) { return Hub::View::rect_fitting_frame(frame_size, target_size); }

private:
    virtual void bookmarks_changed() override { update_bookmark_action(); }

    void update_view_actions();
    void create_translation_actions();
    void update_translation_actions();

#ifndef LADYBIRD_HUB_IN_OWN_PROCESS
    Hub::View* m_view { nullptr };
#endif

    RefPtr<Action> m_navigate_back_action;
    RefPtr<Action> m_navigate_forward_action;
    RefPtr<Action> m_toggle_bookmark_action;
    RefPtr<Action> m_reset_zoom_action;
    RefPtr<Action> m_copy_link_to_highlight_action;
    Optional<URL::URL> m_link_to_highlight;
    RefPtr<Action> m_translate_action;
    RefPtr<Menu> m_translation_menu;
    // One of each per target language, in the order of the state's translation targets.
    Vector<NonnullRefPtr<Action>> m_translate_to_actions;
    Vector<NonnullRefPtr<Action>> m_download_languages_actions;
    RefPtr<Action> m_show_original_action;
    RefPtr<Action> m_prefer_higher_quality_action;
    RefPtr<Menu> m_context_menu;

    u64 m_view_id { 0 };
    WebView::IsPrivate m_is_private { WebView::IsPrivate::No };
    double m_device_pixel_ratio { 1.0 };
    Optional<u64> m_display_id;
    Optional<SessionTabId> m_session_tab_id;
    Optional<String> m_favicon_hash;

    // The external URLs that the user asked for, one at a time, so that their prompts do not overlap.
    struct PendingExternalURL {
        URL::URL url;
        // NB: The page the user asked from, which the prompt names even if the view goes on to another page meanwhile.
        URL::Origin initiator_origin;
        Function<void()> on_handler_unavailable;
    };
    void open_external_url_from_user_input(URL::URL const&, Function<void()> on_handler_unavailable = {});
    void open_next_external_url();
    void open_external_url_with_handler(PendingExternalURL, RefPtr<WebView::ExternalURLHandler>);
    void finish_external_url();
    Queue<PendingExternalURL> m_pending_external_urls;
    bool m_external_url_pending { false };
    bool m_scales_frames_to_fit { false };
    Hub::ViewState m_state;
    HashMap<u64, NavigationListener> m_navigation_listeners;
    HashMap<u64, NonnullRefPtr<Core::Promise<ByteString>>> m_pending_selected_text;
    HashMap<u64, Function<void(bool)>> m_pending_close_preflights;
    HashMap<u64, NonnullRefPtr<Core::Promise<String>>> m_pending_page_dumps;
    u64 m_next_request_id { 1 };
    u64 m_next_navigation_listener_id { 1 };
    WebView::PageBackingStores m_backing_stores;
    u64 m_backing_stores_page_id { 0 };

    // What only the UI can hold of the input events that the Hub has yet to finish with.
    HashMap<u64, OwnPtr<Web::BrowserInputData>> m_input_in_hub;
    u64 m_next_input_id { 1 };

public:
    decltype(Hub::View::on_load_start) on_load_start;
    decltype(Hub::View::on_load_finish) on_load_finish;
    Function<void()> on_load_from_user_input;
    decltype(Hub::View::on_ready_to_paint) on_ready_to_paint;
    decltype(Hub::View::on_performance_stats) on_performance_stats;
    Function<void(Web::HTML::ActivateTab, Web::HTML::WebViewHints, u64 view_id)> on_open_new_view;
    decltype(Hub::View::on_activate_tab) on_activate_tab;
    decltype(Hub::View::on_close) on_close;
    decltype(Hub::View::on_link_hover) on_link_hover;
    decltype(Hub::View::on_link_unhover) on_link_unhover;
    decltype(Hub::View::on_title_change) on_title_change;
    decltype(Hub::View::on_url_change) on_url_change;
    decltype(Hub::View::on_loading_state_change) on_loading_state_change;
    decltype(Hub::View::on_top_level_navigation_commit) on_top_level_navigation_commit;
    decltype(Hub::View::on_browser_history_traversal_complete) on_browser_history_traversal_complete;
    decltype(Hub::View::on_request_dictionary_lookup) on_request_dictionary_lookup;
    decltype(Hub::View::on_favicon_change) on_favicon_change;
    decltype(Hub::View::on_cursor_change) on_cursor_change;
    decltype(Hub::View::on_input_method_state_change) on_input_method_state_change;
    decltype(Hub::View::on_enter_tooltip_area) on_enter_tooltip_area;
    decltype(Hub::View::on_leave_tooltip_area) on_leave_tooltip_area;
    decltype(Hub::View::on_content_blocking_change) on_content_blocking_change;
    decltype(Hub::View::on_request_alert) on_request_alert;
    decltype(Hub::View::on_request_before_unload) on_request_before_unload;
    decltype(Hub::View::on_request_confirm) on_request_confirm;
    decltype(Hub::View::on_request_prompt) on_request_prompt;
    decltype(Hub::View::on_request_set_prompt_text) on_request_set_prompt_text;
    decltype(Hub::View::on_before_browser_initiated_navigation) on_before_browser_initiated_navigation;
    Function<void(URL::URL const&, URL::Origin const&, String const& application_name, Function<void(bool)>)> on_request_external_url_confirmation;
    decltype(Hub::View::on_request_accept_dialog) on_request_accept_dialog;
    decltype(Hub::View::on_request_website_issue_report) on_request_website_issue_report;
    decltype(Hub::View::on_request_screenshot) on_request_screenshot;
    decltype(Hub::View::on_request_dismiss_dialog) on_request_dismiss_dialog;
    decltype(Hub::View::on_accessibility_tree_received) on_accessibility_tree_received;
    decltype(Hub::View::on_accessibility_tree_changed) on_accessibility_tree_changed;
    decltype(Hub::View::on_accessibility_focus_changed) on_accessibility_focus_changed;
    decltype(Hub::View::on_restore_window) on_restore_window;
    decltype(Hub::View::on_reposition_window) on_reposition_window;
    decltype(Hub::View::on_resize_window) on_resize_window;
    decltype(Hub::View::on_maximize_window) on_maximize_window;
    decltype(Hub::View::on_minimize_window) on_minimize_window;
    decltype(Hub::View::on_fullscreen_window) on_fullscreen_window;
    decltype(Hub::View::on_exit_fullscreen_window) on_exit_fullscreen_window;
    decltype(Hub::View::on_request_color_picker) on_request_color_picker;
    decltype(Hub::View::on_request_date_picker) on_request_date_picker;
    decltype(Hub::View::on_request_permission) on_request_permission;
    decltype(Hub::View::on_cancel_permission_prompt) on_cancel_permission_prompt;
    decltype(Hub::View::on_tab_indicator_change) on_tab_indicator_change;
    Function<void(Hub::ViewState const& previous_state)> on_state_change;
    decltype(Hub::View::on_request_file_picker) on_request_file_picker;
    decltype(Hub::View::on_request_select_dropdown) on_request_select_dropdown;
    decltype(Hub::View::on_finish_handling_key_event) on_finish_handling_key_event;
    decltype(Hub::View::on_finish_handling_mouse_event) on_finish_handling_mouse_event;
    decltype(Hub::View::on_picture_in_picture_control_activated) on_picture_in_picture_control_activated;
    decltype(Hub::View::on_finish_handling_drag_event) on_finish_handling_drag_event;
    decltype(Hub::View::on_find_in_page) on_find_in_page;
    decltype(Hub::View::on_page_background_color_change) on_page_background_color_change;
    decltype(Hub::View::on_screen_wake_lock_state_changed) on_screen_wake_lock_state_changed;
    decltype(Hub::View::on_web_content_crashed) on_web_content_crashed;
    decltype(Hub::View::on_crash_overlay_state_change) on_crash_overlay_state_change;
    decltype(Hub::View::on_crash_report_saved) on_crash_report_saved;
};

}
