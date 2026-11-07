/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/HubConnection.h>
#include <UI/Qt/RemoteView.h>

namespace Ladybird {

static HubConnection* s_the;

HubConnection::HubConnection(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionToServer<HubClientEndpoint, HubServerEndpoint>(*this, move(transport))
{
    s_the = this;
}

HubConnection::~HubConnection()
{
    s_the = nullptr;
}

HubConnection& HubConnection::the()
{
    return *s_the;
}

HubConnection* HubConnection::the_if_connected()
{
    return s_the;
}

void HubConnection::die()
{
}

void HubConnection::view_install_backing_stores(u64 view_id, u64 page_id, Vector<i32> bitmap_ids, Vector<Gfx::SharedImage> backing_stores)
{
    if (auto* view = RemoteView::find(view_id))
        view->did_install_backing_stores(page_id, move(bitmap_ids), move(backing_stores));
    else if (!bitmap_ids.is_empty())
        RemoteView::release_backing_store_to_page(page_id, bitmap_ids[0]);
}

void HubConnection::view_add_backing_stores(u64 view_id, Vector<i32> bitmap_ids, Vector<Gfx::SharedImage> backing_stores)
{
    if (auto* view = RemoteView::find(view_id))
        view->did_add_backing_stores(move(bitmap_ids), move(backing_stores));
}

void HubConnection::view_retire_backing_stores(u64 view_id, Vector<i32> bitmap_ids)
{
    if (auto* view = RemoteView::find(view_id))
        view->did_retire_backing_stores(bitmap_ids);
}

void HubConnection::view_present_backing_store(u64 view_id, u64 page_id, i32 bitmap_id, Gfx::IntSize size, double device_pixel_ratio)
{
    if (auto* view = RemoteView::find(view_id))
        view->did_present_backing_store(page_id, bitmap_id, size, device_pixel_ratio);
    else
        RemoteView::release_backing_store_to_page(page_id, bitmap_id);
}

void HubConnection::view_accept_presented_backing_store(u64 view_id, i32 bitmap_id, Gfx::IntRect damage_rect)
{
    if (auto* view = RemoteView::find(view_id))
        view->did_accept_backing_store(bitmap_id, damage_rect);
}

void HubConnection::view_keep_front_backing_store_as_backup(u64 view_id)
{
    if (auto* view = RemoteView::find(view_id))
        view->did_keep_front_backing_store_as_backup();
}

void HubConnection::view_clear_backup_backing_store(u64 view_id)
{
    if (auto* view = RemoteView::find(view_id))
        view->did_clear_backup_backing_store();
}

void HubConnection::view_clear_backing_stores(u64 view_id)
{
    if (auto* view = RemoteView::find(view_id))
        view->did_clear_backing_stores();
}

void HubConnection::view_finish_handling_key_event(u64 view_id, u64 input_id, Web::KeyEvent event)
{
    if (auto* view = RemoteView::find(view_id))
        view->did_finish_handling_key_event(input_id, move(event));
}

void HubConnection::view_finish_handling_mouse_event(u64 view_id, u64 input_id, Web::MouseEvent event, Web::EventResult result)
{
    if (auto* view = RemoteView::find(view_id))
        view->did_finish_handling_mouse_event(input_id, move(event), result);
}

void HubConnection::view_finish_handling_drag_event(u64 view_id, u64 input_id, Web::DragEvent event)
{
    if (auto* view = RemoteView::find(view_id))
        view->did_finish_handling_drag_event(input_id, move(event));
}

void HubConnection::view_release_input(u64 view_id, u64 input_id)
{
    if (auto* view = RemoteView::find(view_id))
        view->release_input(input_id);
}

void HubConnection::view_open_new_view(u64 view_id, u64 new_view_id, Hub::ViewState, Web::HTML::ActivateTab activate_tab, Web::HTML::WebViewHints hints)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_open_new_view)
        view->on_open_new_view(activate_tab, hints, new_view_id);
    else
        async_destroy_view(new_view_id);
}

void HubConnection::view_favicon_change(u64 view_id, Optional<Gfx::ShareableBitmap> favicon)
{
    auto* view = RemoteView::find(view_id);
    if (!view || !view->on_favicon_change)
        return;
    if (favicon.has_value() && favicon->is_valid())
        view->on_favicon_change(*favicon->bitmap());
    else
        view->on_favicon_change({});
}

void HubConnection::view_request_before_unload(u64 view_id, u64 answer_id, String source)
{
    auto* view = RemoteView::find(view_id);
    if (!view || !view->on_request_before_unload) {
        async_view_answer(view_id, answer_id, true);
        return;
    }
    view->on_request_before_unload(source, [view_id, answer_id](bool answer) {
        if (auto* hub = HubConnection::the_if_connected())
            hub->async_view_answer(view_id, answer_id, answer);
    });
}

void HubConnection::view_request_external_url_confirmation(u64 view_id, u64 answer_id, URL::URL url, URL::Origin initiator_origin, String application_name)
{
    auto* view = RemoteView::find(view_id);
    if (!view || !view->on_request_external_url_confirmation) {
        async_view_answer(view_id, answer_id, false);
        return;
    }
    view->on_request_external_url_confirmation(url, initiator_origin, application_name, [view_id, answer_id](bool answer) {
        if (auto* hub = HubConnection::the_if_connected())
            hub->async_view_answer(view_id, answer_id, answer);
    });
}

void HubConnection::view_request_permission(u64 view_id, u64 answer_id, WebView::PermissionPromptRequest request)
{
    auto* view = RemoteView::find(view_id);
    if (!view || !view->on_request_permission) {
        async_view_answer_permission_request(view_id, answer_id, Web::Permissions::PromptOutcome::Dismissed);
        return;
    }
    view->on_request_permission(request, [view_id, answer_id](Web::Permissions::PromptOutcome outcome) {
        if (auto* hub = HubConnection::the_if_connected())
            hub->async_view_answer_permission_request(view_id, answer_id, outcome);
    });
}

void HubConnection::view_request_date_picker(u64 view_id, Utf16String input_type, Utf16String value, Utf16String minimum, Utf16String maximum)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_request_date_picker)
        view->on_request_date_picker(input_type, value, minimum, maximum);
}

void HubConnection::view_request_dictionary_lookup(u64 view_id, WebView::DictionaryLookup lookup, Gfx::IntPoint position)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_request_dictionary_lookup)
        view->on_request_dictionary_lookup(lookup, position);
}

void HubConnection::view_update_zoom(u64 view_id)
{
    if (auto* view = RemoteView::find(view_id))
        view->did_update_zoom();
}

void HubConnection::view_navigation_load_start(u64 view_id, URL::URL url)
{
    if (auto* view = RemoteView::find(view_id))
        view->did_start_navigation_load(url);
}

void HubConnection::view_navigation_load_finish(u64 view_id, URL::URL url)
{
    if (auto* view = RemoteView::find(view_id))
        view->did_finish_navigation_load(url);
}

void HubConnection::view_state_changed(u64 view_id, Hub::ViewState state)
{
    if (auto* view = RemoteView::find(view_id))
        view->set_state(move(state));
}

void HubConnection::view_load_start(u64 view_id)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_load_start)
        view->on_load_start();
}

void HubConnection::view_load_finish(u64 view_id, URL::URL url)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_load_finish)
        view->on_load_finish(move(url));
}

void HubConnection::view_activate_tab(u64 view_id)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_activate_tab)
        view->on_activate_tab();
}

void HubConnection::view_close(u64 view_id)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_close)
        view->on_close();
}

void HubConnection::view_link_hover(u64 view_id, URL::URL url)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_link_hover)
        view->on_link_hover(move(url));
}

void HubConnection::view_link_unhover(u64 view_id)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_link_unhover)
        view->on_link_unhover();
}

void HubConnection::view_title_change(u64 view_id, Utf16String title)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_title_change)
        view->on_title_change(move(title));
}

void HubConnection::view_url_change(u64 view_id, URL::URL url)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_url_change)
        view->on_url_change(move(url));
}

void HubConnection::view_loading_state_change(u64 view_id, bool is_loading)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_loading_state_change)
        view->on_loading_state_change(move(is_loading));
}

void HubConnection::view_top_level_navigation_commit(u64 view_id)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_top_level_navigation_commit)
        view->on_top_level_navigation_commit();
}

void HubConnection::view_browser_history_traversal_complete(u64 view_id)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_browser_history_traversal_complete)
        view->on_browser_history_traversal_complete();
}

void HubConnection::view_cursor_change(u64 view_id, Gfx::Cursor cursor)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_cursor_change)
        view->on_cursor_change(move(cursor));
}

void HubConnection::view_input_method_state_change(u64 view_id)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_input_method_state_change)
        view->on_input_method_state_change();
}

void HubConnection::view_enter_tooltip_area(u64 view_id, ByteString tooltip)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_enter_tooltip_area)
        view->on_enter_tooltip_area(move(tooltip));
}

void HubConnection::view_leave_tooltip_area(u64 view_id)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_leave_tooltip_area)
        view->on_leave_tooltip_area();
}

void HubConnection::view_content_blocking_change(u64 view_id, WebView::ContentBlockingSnapshot snapshot)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_content_blocking_change)
        view->on_content_blocking_change(move(snapshot));
}

void HubConnection::view_request_alert(u64 view_id, Utf16String message)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_request_alert)
        view->on_request_alert(move(message));
}

void HubConnection::view_request_confirm(u64 view_id, Utf16String message)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_request_confirm)
        view->on_request_confirm(move(message));
}

void HubConnection::view_request_prompt(u64 view_id, Utf16String message, Utf16String default_value)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_request_prompt)
        view->on_request_prompt(move(message), move(default_value));
}

void HubConnection::view_request_set_prompt_text(u64 view_id, Utf16String message)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_request_set_prompt_text)
        view->on_request_set_prompt_text(move(message));
}

void HubConnection::view_before_browser_initiated_navigation(u64 view_id)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_before_browser_initiated_navigation)
        view->on_before_browser_initiated_navigation();
}

void HubConnection::view_request_accept_dialog(u64 view_id)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_request_accept_dialog)
        view->on_request_accept_dialog();
}

void HubConnection::view_request_dismiss_dialog(u64 view_id)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_request_dismiss_dialog)
        view->on_request_dismiss_dialog();
}

void HubConnection::view_request_website_issue_report(u64 view_id)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_request_website_issue_report)
        view->on_request_website_issue_report();
}

void HubConnection::view_request_screenshot(u64 view_id)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_request_screenshot)
        view->on_request_screenshot();
}

void HubConnection::view_accessibility_tree_received(u64 view_id, u64 sequence, Vector<WebView::AccessibilityNodeData> nodes)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_accessibility_tree_received)
        view->on_accessibility_tree_received(move(sequence), move(nodes));
}

void HubConnection::view_accessibility_tree_changed(u64 view_id, u64 base_sequence, u64 sequence, WebView::AccessibilityTreeDelta delta)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_accessibility_tree_changed)
        view->on_accessibility_tree_changed(move(base_sequence), move(sequence), move(delta));
}

void HubConnection::view_accessibility_focus_changed(u64 view_id, i64 node_id)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_accessibility_focus_changed)
        view->on_accessibility_focus_changed(move(node_id));
}

void HubConnection::view_restore_window(u64 view_id)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_restore_window)
        view->on_restore_window();
}

void HubConnection::view_reposition_window(u64 view_id, Gfx::IntPoint position)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_reposition_window)
        view->on_reposition_window(move(position));
}

void HubConnection::view_resize_window(u64 view_id, Gfx::IntSize size)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_resize_window)
        view->on_resize_window(move(size));
}

void HubConnection::view_maximize_window(u64 view_id)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_maximize_window)
        view->on_maximize_window();
}

void HubConnection::view_minimize_window(u64 view_id)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_minimize_window)
        view->on_minimize_window();
}

void HubConnection::view_fullscreen_window(u64 view_id)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_fullscreen_window)
        view->on_fullscreen_window();
}

void HubConnection::view_exit_fullscreen_window(u64 view_id)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_exit_fullscreen_window)
        view->on_exit_fullscreen_window();
}

void HubConnection::view_request_color_picker(u64 view_id, Color current_color)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_request_color_picker)
        view->on_request_color_picker(move(current_color));
}

void HubConnection::view_cancel_permission_prompt(u64 view_id, WebView::PermissionPromptRequest request)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_cancel_permission_prompt)
        view->on_cancel_permission_prompt(move(request));
}

void HubConnection::view_tab_indicator_change(u64 view_id, WebView::TabIndicator indicator, bool visible)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_tab_indicator_change)
        view->on_tab_indicator_change(move(indicator), move(visible));
}

void HubConnection::view_request_file_picker(u64 view_id, Web::HTML::FileFilter accepted_file_types, Web::HTML::AllowMultipleFiles allow_multiple_files)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_request_file_picker)
        view->on_request_file_picker(move(accepted_file_types), move(allow_multiple_files));
}

void HubConnection::view_request_select_dropdown(u64 view_id, Gfx::IntPoint content_position, i32 minimum_width, Vector<Web::HTML::SelectItem> items)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_request_select_dropdown)
        view->on_request_select_dropdown(move(content_position), move(minimum_width), move(items));
}

void HubConnection::view_picture_in_picture_control_activated(u64 view_id, Compositing::PictureInPictureControl control)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_picture_in_picture_control_activated)
        view->on_picture_in_picture_control_activated(move(control));
}

void HubConnection::view_find_in_page(u64 view_id, size_t current_match_index, Optional<size_t> total_match_count)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_find_in_page)
        view->on_find_in_page(move(current_match_index), move(total_match_count));
}

void HubConnection::view_page_background_color_change(u64 view_id, Gfx::Color color)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_page_background_color_change)
        view->on_page_background_color_change(move(color));
}

void HubConnection::view_screen_wake_lock_state_changed(u64 view_id, Web::ScreenWakeLockState state)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_screen_wake_lock_state_changed)
        view->on_screen_wake_lock_state_changed(move(state));
}

void HubConnection::view_web_content_crashed(u64 view_id, WebView::ViewImplementation::WebContentCrashReason reason)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_web_content_crashed)
        view->on_web_content_crashed(move(reason));
}

void HubConnection::view_crash_overlay_state_change(u64 view_id, bool active)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_crash_overlay_state_change)
        view->on_crash_overlay_state_change(move(active));
}

void HubConnection::view_crash_report_saved(u64 view_id)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_crash_report_saved)
        view->on_crash_report_saved();
}

void HubConnection::view_performance_stats(u64 view_id, WebView::TabPerformanceStats stats)
{
    if (auto* view = RemoteView::find(view_id); view && view->on_performance_stats)
        view->on_performance_stats(move(stats));
}

}
