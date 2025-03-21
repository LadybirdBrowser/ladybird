/*
 * Copyright (c) 2020-2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "WebContentClient.h"
#include "Application.h"
#include "ViewImplementation.h"
#include <LibWeb/Cookie/ParsedCookie.h>
#include <LibWebView/CookieJar.h>
#include <LibWebView/HelperProcess.h>

namespace WebView {

HashTable<WebContentClient*> WebContentClient::s_clients;

Optional<ViewImplementation&> WebContentClient::view_for_pid_and_page_id(pid_t pid, u64 page_id)
{
    for (auto* client : s_clients) {
        if (client->m_process_handle.pid == pid)
            return client->view_for_page_id(page_id);
    }
    return {};
}

WebContentClient::WebContentClient(IPC::Transport transport, ViewImplementation& view)
    : IPC::ConnectionToServer<WebContentClientEndpoint, WebContentServerEndpoint>(*this, move(transport))
{
    s_clients.set(this);
    m_views.set(0, &view);
}

WebContentClient::WebContentClient(IPC::Transport transport)
    : IPC::ConnectionToServer<WebContentClientEndpoint, WebContentServerEndpoint>(*this, move(transport))
{
    s_clients.set(this);
}

WebContentClient::~WebContentClient()
{
    s_clients.remove(this);
}

void WebContentClient::die()
{
    // Intentionally empty. Restart is handled at another level.
}

void WebContentClient::assign_view(Badge<Application>, ViewImplementation& view)
{
    VERIFY(m_views.is_empty());
    m_views.set(0, &view);
}

void WebContentClient::register_view(u64 page_id, ViewImplementation& view)
{
    VERIFY(page_id > 0);
    m_views.set(page_id, &view);
}

void WebContentClient::unregister_view(u64 page_id)
{
    m_views.remove(page_id);
    if (m_views.is_empty()) {
        on_web_content_process_crash = nullptr;
        async_close_server();
    }
}

void WebContentClient::did_paint(u64 page_id, Gfx::IntRect rect, i32 bitmap_id)
{
    if (auto view = view_for_page_id(page_id); view.has_value())
        view->server_did_paint({}, bitmap_id, rect.size());
}

void WebContentClient::did_request_new_process_for_navigation(u64 page_id, URL::URL url)
{
    if (auto view = view_for_page_id(page_id); view.has_value())
        view->create_new_process_for_cross_site_navigation(url);
}

void WebContentClient::did_start_loading(u64 page_id, URL::URL url, bool is_redirect)
{
    if (auto process = WebView::Application::the().find_process(m_process_handle.pid); process.has_value())
        process->set_title(OptionalNone {});

    if (auto view = view_for_page_id(page_id); view.has_value()) {
        view->set_url({}, url);

        if (view->on_load_start)
            view->on_load_start(url, is_redirect);
    }
}

void WebContentClient::did_finish_loading(u64 page_id, URL::URL url)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        view->set_url({}, url);

        if (view->on_load_finish)
            view->on_load_finish(url);
    }
}

void WebContentClient::did_finish_test(u64 page_id, String text)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_test_finish)
            view->on_test_finish(text);
    }
}

void WebContentClient::did_set_test_timeout(u64 page_id, double milliseconds)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_set_test_timeout)
            view->on_set_test_timeout(milliseconds);
    }
}

void WebContentClient::did_set_browser_zoom(u64 page_id, double factor)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_set_browser_zoom)
            view->on_set_browser_zoom(factor);
    }
}

void WebContentClient::did_find_in_page(u64 page_id, size_t current_match_index, Optional<size_t> total_match_count)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_find_in_page)
            view->on_find_in_page(current_match_index, total_match_count);
    }
}

void WebContentClient::did_request_refresh(u64 page_id)
{
    if (auto view = view_for_page_id(page_id); view.has_value())
        view->reload();
}

void WebContentClient::did_request_cursor_change(u64 page_id, Gfx::Cursor cursor)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_cursor_change)
            view->on_cursor_change(cursor);
    }
}

void WebContentClient::did_change_title(u64 page_id, ByteString title)
{
    if (auto process = WebView::Application::the().find_process(m_process_handle.pid); process.has_value())
        process->set_title(MUST(String::from_byte_string(title)));

    if (auto view = view_for_page_id(page_id); view.has_value()) {
        auto title_or_url = title.is_empty() ? view->url().to_byte_string() : title;
        view->set_title({}, title_or_url);

        if (view->on_title_change)
            view->on_title_change(title_or_url);
    }
}

void WebContentClient::did_change_url(u64 page_id, URL::URL url)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        view->set_url({}, url);

        if (view->on_url_change)
            view->on_url_change(url);
    }
}

void WebContentClient::did_request_tooltip_override(u64 page_id, Gfx::IntPoint position, ByteString title)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_request_tooltip_override)
            view->on_request_tooltip_override(view->to_widget_position(position), title);
    }
}

void WebContentClient::did_stop_tooltip_override(u64 page_id)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_stop_tooltip_override)
            view->on_stop_tooltip_override();
    }
}

void WebContentClient::did_enter_tooltip_area(u64 page_id, ByteString title)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_enter_tooltip_area)
            view->on_enter_tooltip_area(title);
    }
}

void WebContentClient::did_leave_tooltip_area(u64 page_id)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_leave_tooltip_area)
            view->on_leave_tooltip_area();
    }
}

void WebContentClient::did_hover_link(u64 page_id, URL::URL url)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_link_hover)
            view->on_link_hover(url);
    }
}

void WebContentClient::did_unhover_link(u64 page_id)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_link_unhover)
            view->on_link_unhover();
    }
}

void WebContentClient::did_click_link(u64 page_id, URL::URL url, ByteString target, unsigned modifiers)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_link_click)
            view->on_link_click(url, target, modifiers);
    }
}

void WebContentClient::did_middle_click_link(u64 page_id, URL::URL url, ByteString target, unsigned modifiers)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_link_middle_click)
            view->on_link_middle_click(url, target, modifiers);
    }
}

void WebContentClient::did_request_context_menu(u64 page_id, Gfx::IntPoint content_position)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_context_menu_request)
            view->on_context_menu_request(view->to_widget_position(content_position));
    }
}

void WebContentClient::did_request_link_context_menu(u64 page_id, Gfx::IntPoint content_position, URL::URL url, ByteString, unsigned)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_link_context_menu_request)
            view->on_link_context_menu_request(url, view->to_widget_position(content_position));
    }
}

void WebContentClient::did_request_image_context_menu(u64 page_id, Gfx::IntPoint content_position, URL::URL url, ByteString, unsigned, Optional<Gfx::ShareableBitmap> bitmap)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_image_context_menu_request)
            view->on_image_context_menu_request(url, view->to_widget_position(content_position), bitmap);
    }
}

void WebContentClient::did_request_media_context_menu(u64 page_id, Gfx::IntPoint content_position, ByteString, unsigned, Web::Page::MediaContextMenu menu)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_media_context_menu_request)
            view->on_media_context_menu_request(view->to_widget_position(content_position), menu);
    }
}

void WebContentClient::did_get_source(u64 page_id, URL::URL url, URL::URL base_url, String source)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_received_source)
            view->on_received_source(url, base_url, source);
    }
}

static JsonObject parse_json(StringView json, StringView name)
{
    auto parsed_tree = JsonValue::from_string(json);
    if (parsed_tree.is_error()) {
        dbgln("Unable to parse {}: {}", name, parsed_tree.error());
        return {};
    }

    if (!parsed_tree.value().is_object()) {
        dbgln("Expected {} to be an object: {}", name, parsed_tree.value());
        return {};
    }

    return move(parsed_tree.release_value().as_object());
}

void WebContentClient::did_inspect_dom_tree(u64 page_id, String dom_tree)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_received_dom_tree)
            view->on_received_dom_tree(parse_json(dom_tree, "DOM tree"sv));
    }
}

void WebContentClient::did_inspect_dom_node(u64 page_id, DOMNodeProperties properties)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_received_dom_node_properties)
            view->on_received_dom_node_properties(move(properties));
    }
}

void WebContentClient::did_inspect_accessibility_tree(u64 page_id, String accessibility_tree)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_received_accessibility_tree)
            view->on_received_accessibility_tree(parse_json(accessibility_tree, "accessibility tree"sv));
    }
}

void WebContentClient::did_get_hovered_node_id(u64 page_id, Web::UniqueNodeID node_id)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_received_hovered_node_id)
            view->on_received_hovered_node_id(node_id);
    }
}

void WebContentClient::did_finish_editing_dom_node(u64 page_id, Optional<Web::UniqueNodeID> node_id)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_finshed_editing_dom_node)
            view->on_finshed_editing_dom_node(node_id);
    }
}

void WebContentClient::did_mutate_dom(u64 page_id, Mutation mutation)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_dom_mutation_received)
            view->on_dom_mutation_received(move(mutation));
    }
}

void WebContentClient::did_get_dom_node_html(u64 page_id, String html)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_received_dom_node_html)
            view->on_received_dom_node_html(move(html));
    }
}

void WebContentClient::did_list_style_sheets(u64 page_id, Vector<Web::CSS::StyleSheetIdentifier> stylesheets)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_received_style_sheet_list)
            view->on_received_style_sheet_list(stylesheets);
    }
}

void WebContentClient::did_get_style_sheet_source(u64 page_id, Web::CSS::StyleSheetIdentifier identifier, URL::URL base_url, String source)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_received_style_sheet_source)
            view->on_received_style_sheet_source(identifier, base_url, source);
    }
}

void WebContentClient::did_take_screenshot(u64 page_id, Gfx::ShareableBitmap screenshot)
{
    if (auto view = view_for_page_id(page_id); view.has_value())
        view->did_receive_screenshot({}, screenshot);
}

void WebContentClient::did_get_internal_page_info(u64 page_id, WebView::PageInfoType type, String info)
{
    if (auto view = view_for_page_id(page_id); view.has_value())
        view->did_receive_internal_page_info({}, type, info);
}

void WebContentClient::did_execute_js_console_input(u64 page_id, JsonValue result)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_received_js_console_result)
            view->on_received_js_console_result(move(result));
    }
}

void WebContentClient::did_output_js_console_message(u64 page_id, i32 message_index)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_console_message_available)
            view->on_console_message_available(message_index);
    }
}

void WebContentClient::did_get_styled_js_console_messages(u64 page_id, i32 start_index, Vector<String> message_types, Vector<String> messages)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_received_styled_console_messages)
            view->on_received_styled_console_messages(start_index, message_types, messages);
    }
}

void WebContentClient::did_get_unstyled_js_console_messages(u64 page_id, i32 start_index, Vector<ConsoleOutput> console_output)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_received_unstyled_console_messages)
            view->on_received_unstyled_console_messages(start_index, move(console_output));
    }
}

void WebContentClient::did_request_alert(u64 page_id, String message)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_request_alert)
            view->on_request_alert(message);
    }
}

void WebContentClient::did_request_confirm(u64 page_id, String message)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_request_confirm)
            view->on_request_confirm(message);
    }
}

void WebContentClient::did_request_prompt(u64 page_id, String message, String default_)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_request_prompt)
            view->on_request_prompt(message, default_);
    }
}

void WebContentClient::did_request_set_prompt_text(u64 page_id, String message)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_request_set_prompt_text)
            view->on_request_set_prompt_text(message);
    }
}

void WebContentClient::did_request_accept_dialog(u64 page_id)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_request_accept_dialog)
            view->on_request_accept_dialog();
    }
}

void WebContentClient::did_request_dismiss_dialog(u64 page_id)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_request_dismiss_dialog)
            view->on_request_dismiss_dialog();
    }
}

void WebContentClient::did_change_favicon(u64 page_id, Gfx::ShareableBitmap favicon)
{
    if (!favicon.is_valid()) {
        dbgln("DidChangeFavicon: Received invalid favicon");
        return;
    }

    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_favicon_change)
            view->on_favicon_change(*favicon.bitmap());
    }
}

Messages::WebContentClient::DidRequestAllCookiesResponse WebContentClient::did_request_all_cookies(URL::URL url)
{
    return Application::cookie_jar().get_all_cookies(url);
}

Messages::WebContentClient::DidRequestNamedCookieResponse WebContentClient::did_request_named_cookie(URL::URL url, String name)
{
    return Application::cookie_jar().get_named_cookie(url, name);
}

Messages::WebContentClient::DidRequestCookieResponse WebContentClient::did_request_cookie(URL::URL url, Web::Cookie::Source source)
{
    return Application::cookie_jar().get_cookie(url, source);
}

void WebContentClient::did_set_cookie(URL::URL url, Web::Cookie::ParsedCookie cookie, Web::Cookie::Source source)
{
    Application::cookie_jar().set_cookie(url, cookie, source);
}

void WebContentClient::did_update_cookie(Web::Cookie::Cookie cookie)
{
    Application::cookie_jar().update_cookie(cookie);
}

void WebContentClient::did_expire_cookies_with_time_offset(AK::Duration offset)
{
    Application::cookie_jar().expire_cookies_with_time_offset(offset);
}

Messages::WebContentClient::DidRequestNewWebViewResponse WebContentClient::did_request_new_web_view(u64 page_id, Web::HTML::ActivateTab activate_tab, Web::HTML::WebViewHints hints, Optional<u64> page_index)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_new_web_view)
            return view->on_new_web_view(activate_tab, hints, page_index);
    }

    return String {};
}

void WebContentClient::did_request_activate_tab(u64 page_id)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_activate_tab)
            view->on_activate_tab();
    }
}

void WebContentClient::did_close_browsing_context(u64 page_id)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_close)
            view->on_close();
    }
}

void WebContentClient::did_update_resource_count(u64 page_id, i32 count_waiting)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_resource_status_change)
            view->on_resource_status_change(count_waiting);
    }
}

void WebContentClient::did_request_restore_window(u64 page_id)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_restore_window)
            view->on_restore_window();
    }
}

void WebContentClient::did_request_reposition_window(u64 page_id, Gfx::IntPoint position)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_reposition_window)
            view->on_reposition_window(position);
    }
}

void WebContentClient::did_request_resize_window(u64 page_id, Gfx::IntSize size)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_resize_window)
            view->on_resize_window(size);
    }
}

void WebContentClient::did_request_maximize_window(u64 page_id)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_maximize_window)
            view->on_maximize_window();
    }
}

void WebContentClient::did_request_minimize_window(u64 page_id)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_minimize_window)
            view->on_minimize_window();
    }
}

void WebContentClient::did_request_fullscreen_window(u64 page_id)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_fullscreen_window)
            view->on_fullscreen_window();
    }
}

void WebContentClient::did_request_file(u64 page_id, ByteString path, i32 request_id)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_request_file)
            view->on_request_file(path, request_id);
    }
}

void WebContentClient::did_request_color_picker(u64 page_id, Color current_color)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_request_color_picker)
            view->on_request_color_picker(current_color);
    }
}

void WebContentClient::did_request_file_picker(u64 page_id, Web::HTML::FileFilter accepted_file_types, Web::HTML::AllowMultipleFiles allow_multiple_files)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_request_file_picker)
            view->on_request_file_picker(accepted_file_types, allow_multiple_files);
    }
}

void WebContentClient::did_request_select_dropdown(u64 page_id, Gfx::IntPoint content_position, i32 minimum_width, Vector<Web::HTML::SelectItem> items)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_request_select_dropdown)
            view->on_request_select_dropdown(view->to_widget_position(content_position), minimum_width / view->device_pixel_ratio(), items);
    }
}

void WebContentClient::did_finish_handling_input_event(u64 page_id, Web::EventResult event_result)
{
    if (auto view = view_for_page_id(page_id); view.has_value())
        view->did_finish_handling_input_event({}, event_result);
}

void WebContentClient::did_change_theme_color(u64 page_id, Gfx::Color color)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_theme_color_change)
            view->on_theme_color_change(color);
    }
}

void WebContentClient::did_insert_clipboard_entry(u64 page_id, String data, String presentation_style, String mime_type)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_insert_clipboard_entry)
            view->on_insert_clipboard_entry(data, presentation_style, mime_type);
    }
}

void WebContentClient::did_change_audio_play_state(u64 page_id, Web::HTML::AudioPlayState play_state)
{
    if (auto view = view_for_page_id(page_id); view.has_value())
        view->did_change_audio_play_state({}, play_state);
}

void WebContentClient::did_update_navigation_buttons_state(u64 page_id, bool back_enabled, bool forward_enabled)
{
    if (auto view = view_for_page_id(page_id); view.has_value())
        view->did_update_navigation_buttons_state({}, back_enabled, forward_enabled);
}

void WebContentClient::did_allocate_backing_stores(u64 page_id, i32 front_bitmap_id, Gfx::ShareableBitmap front_bitmap, i32 back_bitmap_id, Gfx::ShareableBitmap back_bitmap)
{
    if (auto view = view_for_page_id(page_id); view.has_value())
        view->did_allocate_backing_stores({}, front_bitmap_id, front_bitmap, back_bitmap_id, back_bitmap);
}

Messages::WebContentClient::RequestWorkerAgentResponse WebContentClient::request_worker_agent(u64 page_id)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        auto worker_client = MUST(WebView::launch_web_worker_process());
        return worker_client->clone_transport();
    }

    return IPC::File {};
}

void WebContentClient::update_process_statistics(u64 page_id)
{
    if (auto view = view_for_page_id(page_id); view.has_value())
        WebView::Application::the().send_updated_process_statistics_to_view(*view);
}

void WebContentClient::request_current_settings(u64 page_id)
{
    if (auto view = view_for_page_id(page_id); view.has_value())
        WebView::Application::the().send_current_settings_to_view(*view);
}

void WebContentClient::restore_default_settings(u64 page_id)
{
    WebView::Application::settings().restore_defaults();
    request_current_settings(page_id);
}

void WebContentClient::set_new_tab_page_url(u64 page_id, URL::URL new_tab_page_url)
{
    WebView::Application::settings().set_new_tab_page_url(move(new_tab_page_url));
    request_current_settings(page_id);
}

void WebContentClient::request_available_search_engines(u64 page_id)
{
    if (auto view = view_for_page_id(page_id); view.has_value())
        WebView::Application::the().send_available_search_engines_to_view(*view);
}

void WebContentClient::set_search_engine(u64 page_id, Optional<String> search_engine)
{
    WebView::Application::settings().set_search_engine(search_engine.map([](auto const& search_engine) {
        return search_engine.bytes_as_string_view();
    }));

    request_current_settings(page_id);
}

Optional<ViewImplementation&> WebContentClient::view_for_page_id(u64 page_id, SourceLocation location)
{
    // Don't bother logging anything for the spare WebContent process. It will only receive a load notification for about:blank.
    if (m_views.is_empty())
        return {};

    if (auto view = m_views.get(page_id); view.has_value())
        return *view.value();

    dbgln("WebContentClient::{}: Did not find a page with ID {}", location.function_name(), page_id);
    return {};
}

}
