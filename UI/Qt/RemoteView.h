/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <Hub/View.h>
#include <LibWebView/HistoryStore.h>
#include <UI/Qt/HubConnection.h>

namespace Ladybird {

// The UI's side of a tab's view. The view itself lives in the Hub. This holds what the UI needs to show it, and offers
// the rest of the UI the calls and callbacks that it takes to use the view.
class RemoteView : public Hub::ViewHost {
public:
    using InputMethodState = WebView::ViewImplementation::InputMethodState;
    using NavigationListener = WebView::ViewImplementation::NavigationListener;
    using ScreenRects = WebView::ViewImplementation::ScreenRects;
    using ScreenshotType = WebView::ViewImplementation::ScreenshotType;
    using SessionHistoryTraversalMenuItem = WebView::ViewImplementation::SessionHistoryTraversalMenuItem;

    explicit RemoteView(Hub::View::InitialState);
    virtual ~RemoteView() override;

    // The UI's side of a view that the UI shows.
    static RemoteView& of(WebView::ViewImplementation const&);
    static RemoteView* find(u64 view_id);

    void set_state(Hub::ViewState state) { m_state = move(state); }

    void did_finish_handling_key_event(u64 input_id, Web::KeyEvent);
    void did_finish_handling_mouse_event(u64 input_id, Web::MouseEvent, Web::EventResult);
    void did_finish_handling_drag_event(u64 input_id, Web::DragEvent);
    void release_input(u64 input_id) { m_input_in_hub.remove(input_id); }

    Hub::View& hub_view() { return *m_view; }
    Hub::View const& hub_view() const { return *m_view; }

    WebView::IsPrivate is_private() const { return m_view->is_private(); }
    WebView::BrowsingSession& session() const { return m_view->session(); }
    URL::Origin permission_origin() const { return *m_state.permission_origin; }
    Vector<WebView::SitePermissionRow> site_permission_rows() const { return m_state.site_permission_rows; }
    void set_site_permission(URL::Origin const& origin, Web::Permissions::PermissionName permission_name, Optional<WebView::PermissionDecision> decision) { m_view->set_site_permission(origin, permission_name, decision); }
    void reset_site_permissions(URL::Origin const& origin) { HubConnection::the().async_view_reset_site_permissions(view_id(), origin); }
    void did_show_permission_prompt(WebView::PermissionPromptRequest const& permission_prompt_request) { m_view->did_show_permission_prompt(permission_prompt_request); }
    u64 view_id() const { return m_view->view_id(); }
    pid_t web_content_pid() const { return m_state.web_content_pid; }
    WebView::CanonicalTraversable& traversable() const { return m_view->traversable(); }
    URL::URL url() const { return m_state.url; }
    URL::URL const& displayed_url() const { return m_state.displayed_url; }
    Utf16String const& title() const { return m_state.title; }
    Optional<String> const& favicon_hash() const { return m_state.favicon_hash; }
    String const& handle() const { return m_state.handle; }
    void set_window_position(Gfx::IntPoint position) { HubConnection::the().async_view_set_window_position(view_id(), position); }
    void set_window_size(Gfx::IntSize size) { HubConnection::the().async_view_set_window_size(view_id(), size); }
    void set_system_visibility_state(Web::HTML::VisibilityState visibility_state) { HubConnection::the().async_view_set_system_visibility_state(view_id(), move(visibility_state)); }
    void set_presents_from_cpu_memory(bool presents_from_cpu_memory) { HubConnection::the().async_view_set_presents_from_cpu_memory(view_id(), presents_from_cpu_memory); }
    void set_has_system_focus(bool has_system_focus) { HubConnection::the().async_view_set_has_system_focus(view_id(), has_system_focus); }
    void load(URL::URL const& url, Web::Bindings::NavigationHistoryBehavior navigation_history_behavior = Web::Bindings::NavigationHistoryBehavior::Auto) { HubConnection::the().async_view_load(view_id(), url, navigation_history_behavior); }
    void load_from_user_input(URL::URL const& url) { HubConnection::the().async_view_load_url_from_user_input(view_id(), url); }
    void load_from_user_input(StringView input) { HubConnection::the().async_view_load_from_user_input(view_id(), input, {}); }
    void load_from_user_input(StringView input, Optional<URL::URL> fallback_url) { HubConnection::the().async_view_load_from_user_input(view_id(), input, move(fallback_url)); }
    void open_url_in_new_tab(URL::URL const& url, Web::HTML::ActivateTab activate_tab) { HubConnection::the().async_view_open_url_in_new_tab(view_id(), url, move(activate_tab)); }
    void open_url_in_new_window(URL::URL const& url, WebView::IsPrivate is_private) { HubConnection::the().async_view_open_url_in_new_window(view_id(), url, move(is_private)); }
    void load(Web::HTML::PreparedNavigationDescriptor navigation) { m_view->load(move(navigation)); }
    void open_navigation_in_new_tab(Web::HTML::PreparedNavigationDescriptor navigation, Web::HTML::ActivateTab activate_tab) { m_view->open_navigation_in_new_tab(move(navigation), move(activate_tab)); }
    void open_navigation_in_new_window(Web::HTML::PreparedNavigationDescriptor navigation, WebView::IsPrivate is_private) { m_view->open_navigation_in_new_window(move(navigation), move(is_private)); }
    void set_next_history_visit_transition(WebView::HistoryVisitTransition transition) { HubConnection::the().async_view_set_next_history_visit_transition(view_id(), move(transition)); }
    void load_html(StringView html) { HubConnection::the().async_view_load_html(view_id(), html); }
    void load_navigation_error_page(StringView html) { HubConnection::the().async_view_load_navigation_error_page(view_id(), html); }
    void reload(WebView::BypassCache bypass_cache = WebView::BypassCache::No) { HubConnection::the().async_view_reload(view_id(), move(bypass_cache)); }
    void stop_loading() { HubConnection::the().async_view_stop_loading(view_id()); }
    bool is_loading() const { return m_state.is_loading; }
    bool crash_overlay_active() const { return m_state.crash_overlay_active; }
    String crash_overlay_failed_url() const { return m_state.crash_overlay_failed_url; }
    Optional<ByteString> crash_report_name() const { return m_state.crash_report_name; }
    Optional<String> crash_report_website() const { return m_state.crash_report_website; }
    void traverse_the_history_to_step(i32 step, WebView::CheckForCancelation check_for_cancelation = WebView::CheckForCancelation::Yes, Function<void()> on_ready = nullptr) { m_view->traverse_the_history_to_step(step, move(check_for_cancelation), move(on_ready)); }
    Vector<WebView::HistoryEntry> recent_history_entries(size_t limit) const { return m_view->recent_history_entries(limit); }
    Optional<WebView::SessionTabId> session_tab_id() const { return m_view->session_tab_id(); }
    void set_session_tab_id(WebView::SessionTabId session_tab_id) { m_view->set_session_tab_id(move(session_tab_id)); }
    void clear_session_tab_id() { m_view->clear_session_tab_id(); }
    void zoom_in() { HubConnection::the().async_view_zoom_in(view_id()); }
    void zoom_out() { HubConnection::the().async_view_zoom_out(view_id()); }
    void reset_zoom() { HubConnection::the().async_view_reset_zoom(view_id()); }
    double zoom_level() const { return m_state.zoom_level; }
    double device_pixel_ratio() const { return m_view->device_pixel_ratio(); }
    Optional<u64> display_id() const { return m_view->display_id(); }
    void set_display_metadata(Optional<u64> display_id, double maximum_frames_per_second) { HubConnection::the().async_view_set_display_metadata(view_id(), move(display_id), maximum_frames_per_second); }
    void update_screen_rects() { HubConnection::the().async_view_update_screen_rects(view_id()); }
    void set_scales_frames_to_fit(bool scales_frames_to_fit) { HubConnection::the().async_view_set_scales_frames_to_fit(view_id(), scales_frames_to_fit); }
    void enable_picture_in_picture_controls() { HubConnection::the().async_view_enable_picture_in_picture_controls(view_id()); }
    void enqueue_input_event(Web::InputEvent);
    String force_dark_host() const { return m_state.force_dark_host; }
    void update_interface_color_scheme() { HubConnection::the().async_view_update_interface_color_scheme(view_id()); }
    void update_system_theme(Core::AnonymousBuffer const& anonymous_buffer) { m_view->update_system_theme(anonymous_buffer); }
    NonnullRefPtr<Core::Promise<ByteString>> selected_text() { return m_view->selected_text(); }
    bool look_up_selected_text_at(Gfx::IntPoint widget_position) { return m_view->look_up_selected_text_at(move(widget_position)); }
    void find_in_page(Utf16String const& query, CaseSensitivity case_sensitivity = CaseSensitivity::CaseInsensitive, bool highlight_all_matches = false) { HubConnection::the().async_view_find_in_page(view_id(), query, move(case_sensitivity), highlight_all_matches); }
    void find_in_page_next_match() { HubConnection::the().async_view_find_in_page_next_match(view_id()); }
    void find_in_page_previous_match() { HubConnection::the().async_view_find_in_page_previous_match(view_id()); }
    void find_in_page_end() { HubConnection::the().async_view_find_in_page_end(view_id()); }
    bool can_report_website_issue() const { return m_state.can_report_website_issue; }
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
    void file_picker_closed(Vector<Web::HTML::SelectedFile> selected_files) { m_view->file_picker_closed(move(selected_files)); }
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
    WebView::PageBackingStores const& backing_stores() const { return m_view->backing_stores(); }
    void take_and_save_screenshot(ScreenshotType screenshot_type) { HubConnection::the().async_view_take_and_save_screenshot(view_id(), move(screenshot_type)); }
    void save_screenshot(Gfx::Bitmap const& bitmap) { m_view->save_screenshot(bitmap); }
    void request_close() { HubConnection::the().async_view_request_close(view_id()); }
    void request_close_preflight(Function<void(bool)> on_complete) { m_view->request_close_preflight(move(on_complete)); }
    bool needs_beforeunload_check() const { return m_view->needs_beforeunload_check(); }
    u64 add_navigation_listener(NavigationListener navigation_listener) { return m_view->add_navigation_listener(move(navigation_listener)); }
    void remove_navigation_listener(u64 listener_id) { m_view->remove_navigation_listener(listener_id); }
    WebView::Menu& bookmarks_bar_context_menu() { return m_view->bookmarks_bar_context_menu(); }
    WebView::Menu& bookmark_context_menu() { return m_view->bookmark_context_menu(); }
    WebView::Menu& bookmark_folder_context_menu() { return m_view->bookmark_folder_context_menu(); }
    WebView::Action& navigate_back_action() { return m_view->navigate_back_action(); }
    WebView::Action& navigate_forward_action() { return m_view->navigate_forward_action(); }
    WebView::Action& toggle_bookmark_action() { return m_view->toggle_bookmark_action(); }
    WebView::Action& reset_zoom_action() { return m_view->reset_zoom_action(); }
    WebView::PageTranslation& page_translation() { return m_view->page_translation(); }
    WebView::WebContentPage& page() const { return m_view->page(); }

    Vector<SessionHistoryTraversalMenuItem> session_history_traversal_menu_items(int direction) const { return m_view->session_history_traversal_menu_items(direction); }
    WebView::SessionHistorySnapshot session_history_snapshot() const { return m_view->session_history_snapshot(); }
    ErrorOr<void> restore_session_history_from_snapshot(WebView::SessionHistorySnapshot snapshot) { return m_view->restore_session_history_from_snapshot(move(snapshot)); }
    void exit_fullscreen() { HubConnection::the().async_view_exit_fullscreen(view_id()); }
    void deny_fullscreen_window() { HubConnection::the().async_view_deny_fullscreen_window(view_id()); }
    void set_is_fullscreen(Web::ViewportIsFullscreen is_fullscreen) { HubConnection::the().async_view_set_is_fullscreen(view_id(), is_fullscreen); }
    Function<void()> prepare_for_close_without_prompting() { return m_view->prepare_for_close_without_prompting(); }
    Function<void()> prepare_for_immediate_close() { return m_view->prepare_for_immediate_close(); }

    bool scales_frames_to_fit() const { return m_view->scales_frames_to_fit(); }

    // ^Hub::ViewHost
    virtual void release_backing_store(i32 bitmap_id) override { m_view->release_backing_store_to_page(bitmap_id); }

protected:
    void initialize_tab(Web::HTML::VisibilityState state, Optional<WebView::CanonicalTraversable&> traversable = {})
    {
        m_view->initialize_tab(state, traversable);
        m_state = m_view->current_state();
    }
    void handle_resize() { m_view->handle_resize(); }
    void set_owner_view(WebView::ViewImplementation const& owner_view) { m_view->set_owner_view(owner_view); }
    void set_page_background_color_to_system_canvas(bool dark) { m_view->set_page_background_color_to_system_canvas(dark); }
    WebView::MicrophoneAuthorizer& microphone_authorizer() const { return m_view->microphone_authorizer(); }
    static Gfx::IntRect rect_fitting_frame(Gfx::IntSize frame_size, Gfx::IntSize target_size) { return Hub::View::rect_fitting_frame(frame_size, target_size); }

private:
    NonnullOwnPtr<Hub::View> m_view;
    Hub::ViewState m_state;

    // What only the UI can hold of the input events that the Hub has yet to finish with.
    HashMap<u64, OwnPtr<Web::BrowserInputData>> m_input_in_hub;
    u64 m_next_input_id { 1 };

public:
    decltype(Hub::View::on_load_start) on_load_start;
    decltype(Hub::View::on_load_finish) on_load_finish;
    decltype(Hub::View::on_ready_to_paint)& on_ready_to_paint { m_view->on_ready_to_paint };
    decltype(Hub::View::on_performance_stats) on_performance_stats;
    decltype(Hub::View::on_new_web_view)& on_new_web_view { m_view->on_new_web_view };
    decltype(Hub::View::on_activate_tab) on_activate_tab;
    decltype(Hub::View::on_close) on_close;
    decltype(Hub::View::on_link_hover) on_link_hover;
    decltype(Hub::View::on_link_unhover) on_link_unhover;
    decltype(Hub::View::on_title_change) on_title_change;
    decltype(Hub::View::on_url_change) on_url_change;
    decltype(Hub::View::on_loading_state_change) on_loading_state_change;
    decltype(Hub::View::on_top_level_navigation_commit) on_top_level_navigation_commit;
    decltype(Hub::View::on_browser_history_traversal_complete) on_browser_history_traversal_complete;
    decltype(Hub::View::on_request_dictionary_lookup)& on_request_dictionary_lookup { m_view->on_request_dictionary_lookup };
    decltype(Hub::View::on_favicon_change)& on_favicon_change { m_view->on_favicon_change };
    decltype(Hub::View::on_cursor_change) on_cursor_change;
    decltype(Hub::View::on_input_method_state_change) on_input_method_state_change;
    decltype(Hub::View::on_enter_tooltip_area) on_enter_tooltip_area;
    decltype(Hub::View::on_leave_tooltip_area) on_leave_tooltip_area;
    decltype(Hub::View::on_content_blocking_change) on_content_blocking_change;
    decltype(Hub::View::on_request_alert) on_request_alert;
    decltype(Hub::View::on_request_before_unload)& on_request_before_unload { m_view->on_request_before_unload };
    decltype(Hub::View::on_request_confirm) on_request_confirm;
    decltype(Hub::View::on_request_prompt) on_request_prompt;
    decltype(Hub::View::on_request_set_prompt_text) on_request_set_prompt_text;
    decltype(Hub::View::on_before_browser_initiated_navigation) on_before_browser_initiated_navigation;
    decltype(Hub::View::on_request_external_url_confirmation)& on_request_external_url_confirmation { m_view->on_request_external_url_confirmation };
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
    decltype(Hub::View::on_request_date_picker)& on_request_date_picker { m_view->on_request_date_picker };
    decltype(Hub::View::on_request_permission)& on_request_permission { m_view->on_request_permission };
    decltype(Hub::View::on_cancel_permission_prompt) on_cancel_permission_prompt;
    decltype(Hub::View::on_tab_indicator_change) on_tab_indicator_change;
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
