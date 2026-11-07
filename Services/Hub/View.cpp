/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Hub/ConnectionFromUI.h>
#include <Hub/View.h>

namespace Hub {

View::View(ViewHost& host, InitialState initial_state)
    : ViewImplementation(initial_state.is_private)
    , m_host(&host)
{
    m_device_pixel_ratio = initial_state.device_pixel_ratio;
    m_maximum_frames_per_second = initial_state.maximum_frames_per_second;
    m_display_id = initial_state.display_id;
    forward_callbacks_to_ui();
}

void View::forward_callbacks_to_ui()
{
    on_load_start = [this] {
        if (auto* ui = ConnectionFromUI::the())
            ui->async_view_load_start(view_id());
    };

    on_load_finish = [this](auto const&... arguments) {
        if (auto* ui = ConnectionFromUI::the())
            ui->async_view_load_finish(view_id(), arguments...);
    };

    on_activate_tab = [this] {
        if (auto* ui = ConnectionFromUI::the())
            ui->async_view_activate_tab(view_id());
    };

    on_close = [this] {
        if (auto* ui = ConnectionFromUI::the())
            ui->async_view_close(view_id());
    };

    on_link_hover = [this](auto const&... arguments) {
        if (auto* ui = ConnectionFromUI::the())
            ui->async_view_link_hover(view_id(), arguments...);
    };

    on_link_unhover = [this] {
        if (auto* ui = ConnectionFromUI::the())
            ui->async_view_link_unhover(view_id());
    };

    on_title_change = [this](auto const&... arguments) {
        if (auto* ui = ConnectionFromUI::the())
            ui->async_view_title_change(view_id(), arguments...);
    };

    on_url_change = [this](auto const&... arguments) {
        if (auto* ui = ConnectionFromUI::the())
            ui->async_view_url_change(view_id(), arguments...);
    };

    on_loading_state_change = [this](auto const&... arguments) {
        if (auto* ui = ConnectionFromUI::the())
            ui->async_view_loading_state_change(view_id(), arguments...);
    };

    on_top_level_navigation_commit = [this] {
        if (auto* ui = ConnectionFromUI::the())
            ui->async_view_top_level_navigation_commit(view_id());
    };

    on_browser_history_traversal_complete = [this] {
        if (auto* ui = ConnectionFromUI::the())
            ui->async_view_browser_history_traversal_complete(view_id());
    };

    on_cursor_change = [this](auto const&... arguments) {
        if (auto* ui = ConnectionFromUI::the())
            ui->async_view_cursor_change(view_id(), arguments...);
    };

    on_input_method_state_change = [this] {
        if (auto* ui = ConnectionFromUI::the())
            ui->async_view_input_method_state_change(view_id());
    };

    on_enter_tooltip_area = [this](auto const&... arguments) {
        if (auto* ui = ConnectionFromUI::the())
            ui->async_view_enter_tooltip_area(view_id(), arguments...);
    };

    on_leave_tooltip_area = [this] {
        if (auto* ui = ConnectionFromUI::the())
            ui->async_view_leave_tooltip_area(view_id());
    };

    on_content_blocking_change = [this](auto const&... arguments) {
        if (auto* ui = ConnectionFromUI::the())
            ui->async_view_content_blocking_change(view_id(), arguments...);
    };

    on_request_alert = [this](auto const&... arguments) {
        if (auto* ui = ConnectionFromUI::the())
            ui->async_view_request_alert(view_id(), arguments...);
    };

    on_request_confirm = [this](auto const&... arguments) {
        if (auto* ui = ConnectionFromUI::the())
            ui->async_view_request_confirm(view_id(), arguments...);
    };

    on_request_prompt = [this](auto const&... arguments) {
        if (auto* ui = ConnectionFromUI::the())
            ui->async_view_request_prompt(view_id(), arguments...);
    };

    on_request_set_prompt_text = [this](auto const&... arguments) {
        if (auto* ui = ConnectionFromUI::the())
            ui->async_view_request_set_prompt_text(view_id(), arguments...);
    };

    on_before_browser_initiated_navigation = [this] {
        if (auto* ui = ConnectionFromUI::the())
            ui->async_view_before_browser_initiated_navigation(view_id());
    };

    on_request_accept_dialog = [this] {
        if (auto* ui = ConnectionFromUI::the())
            ui->async_view_request_accept_dialog(view_id());
    };

    on_request_dismiss_dialog = [this] {
        if (auto* ui = ConnectionFromUI::the())
            ui->async_view_request_dismiss_dialog(view_id());
    };

    on_request_website_issue_report = [this] {
        if (auto* ui = ConnectionFromUI::the())
            ui->async_view_request_website_issue_report(view_id());
    };

    on_request_screenshot = [this] {
        if (auto* ui = ConnectionFromUI::the())
            ui->async_view_request_screenshot(view_id());
    };

    on_accessibility_tree_received = [this](auto const&... arguments) {
        if (auto* ui = ConnectionFromUI::the())
            ui->async_view_accessibility_tree_received(view_id(), arguments...);
    };

    on_accessibility_tree_changed = [this](auto const&... arguments) {
        if (auto* ui = ConnectionFromUI::the())
            ui->async_view_accessibility_tree_changed(view_id(), arguments...);
    };

    on_accessibility_focus_changed = [this](auto const&... arguments) {
        if (auto* ui = ConnectionFromUI::the())
            ui->async_view_accessibility_focus_changed(view_id(), arguments...);
    };

    on_restore_window = [this] {
        if (auto* ui = ConnectionFromUI::the())
            ui->async_view_restore_window(view_id());
    };

    on_reposition_window = [this](auto const&... arguments) {
        if (auto* ui = ConnectionFromUI::the())
            ui->async_view_reposition_window(view_id(), arguments...);
    };

    on_resize_window = [this](auto const&... arguments) {
        if (auto* ui = ConnectionFromUI::the())
            ui->async_view_resize_window(view_id(), arguments...);
    };

    on_maximize_window = [this] {
        if (auto* ui = ConnectionFromUI::the())
            ui->async_view_maximize_window(view_id());
    };

    on_minimize_window = [this] {
        if (auto* ui = ConnectionFromUI::the())
            ui->async_view_minimize_window(view_id());
    };

    on_fullscreen_window = [this] {
        if (auto* ui = ConnectionFromUI::the())
            ui->async_view_fullscreen_window(view_id());
    };

    on_exit_fullscreen_window = [this] {
        if (auto* ui = ConnectionFromUI::the())
            ui->async_view_exit_fullscreen_window(view_id());
    };

    on_request_color_picker = [this](auto const&... arguments) {
        if (auto* ui = ConnectionFromUI::the())
            ui->async_view_request_color_picker(view_id(), arguments...);
    };

    on_cancel_permission_prompt = [this](auto const&... arguments) {
        if (auto* ui = ConnectionFromUI::the())
            ui->async_view_cancel_permission_prompt(view_id(), arguments...);
    };

    on_tab_indicator_change = [this](auto const&... arguments) {
        if (auto* ui = ConnectionFromUI::the())
            ui->async_view_tab_indicator_change(view_id(), arguments...);
    };

    on_request_file_picker = [this](auto const&... arguments) {
        if (auto* ui = ConnectionFromUI::the())
            ui->async_view_request_file_picker(view_id(), arguments...);
    };

    on_request_select_dropdown = [this](auto const&... arguments) {
        if (auto* ui = ConnectionFromUI::the())
            ui->async_view_request_select_dropdown(view_id(), arguments...);
    };

    on_picture_in_picture_control_activated = [this](auto const&... arguments) {
        if (auto* ui = ConnectionFromUI::the())
            ui->async_view_picture_in_picture_control_activated(view_id(), arguments...);
    };

    on_find_in_page = [this](auto const&... arguments) {
        if (auto* ui = ConnectionFromUI::the())
            ui->async_view_find_in_page(view_id(), arguments...);
    };

    on_page_background_color_change = [this](auto const&... arguments) {
        if (auto* ui = ConnectionFromUI::the())
            ui->async_view_page_background_color_change(view_id(), arguments...);
    };

    on_screen_wake_lock_state_changed = [this](auto const&... arguments) {
        if (auto* ui = ConnectionFromUI::the())
            ui->async_view_screen_wake_lock_state_changed(view_id(), arguments...);
    };

    on_web_content_crashed = [this](auto const&... arguments) {
        if (auto* ui = ConnectionFromUI::the())
            ui->async_view_web_content_crashed(view_id(), arguments...);
    };

    on_crash_overlay_state_change = [this](auto const&... arguments) {
        if (auto* ui = ConnectionFromUI::the())
            ui->async_view_crash_overlay_state_change(view_id(), arguments...);
    };

    on_crash_report_saved = [this] {
        if (auto* ui = ConnectionFromUI::the())
            ui->async_view_crash_report_saved(view_id());
    };

    on_performance_stats = [this](auto const&... arguments) {
        if (auto* ui = ConnectionFromUI::the())
            ui->async_view_performance_stats(view_id(), arguments...);
    };
}

void View::prepare_page_for_navigable(WebView::WebContentPage& page)
{
    ViewImplementation::prepare_page_for_navigable(page);

    update_compositor_display_metadata(page);
}

Core::AnonymousBuffer View::system_theme()
{
    return m_host ? m_host->system_theme() : Core::AnonymousBuffer {};
}

Optional<View::ScreenRects> View::screen_rects() const
{
    return m_host ? m_host->screen_rects() : Optional<ScreenRects> {};
}

void View::update_zoom()
{
    ViewImplementation::update_zoom();
    if (m_host)
        m_host->update_zoom();
}

Web::DevicePixelSize View::viewport_size() const
{
    return m_host ? m_host->viewport_size() : Web::DevicePixelSize {};
}

void View::show_context_menu(WebView::Menu& menu, Gfx::IntPoint widget_position)
{
    if (m_host)
        m_host->show_context_menu(menu, widget_position);
}

void View::did_accept_presented_backing_store(i32 bitmap_id, Gfx::IntRect damage_rect)
{
    if (m_host)
        m_host->did_accept_presented_backing_store(bitmap_id, damage_rect);
}

void View::release_backing_store(i32 bitmap_id)
{
    if (m_host)
        m_host->release_backing_store(bitmap_id);
    else
        ViewImplementation::release_backing_store(bitmap_id);
}

}
