/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <Hub/HubClientEndpoint.h>
#include <Hub/HubServerEndpoint.h>
#include <LibIPC/ConnectionToServer.h>

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

private:
    virtual void die() override;

    // ^HubClientEndpoint
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
