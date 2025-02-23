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

WebContentClient::~WebContentClient()
{
    s_clients.remove(this);
}

void WebContentClient::die()
{
    // Intentionally empty. Restart is handled at another level.
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

void WebContentClient::did_paint(u64 page_id, Gfx::IntRect const& rect, i32 bitmap_id)
{
    if (auto view = view_for_page_id(page_id); view.has_value())
        view->server_did_paint({}, bitmap_id, rect.size());
}

void WebContentClient::did_start_loading(u64 page_id, URL::URL const& url, bool is_redirect)
{
    if (auto process = WebView::Application::the().find_process(m_process_handle.pid); process.has_value())
        process->set_title(OptionalNone {});

    if (auto view = view_for_page_id(page_id); view.has_value()) {
        view->set_url({}, url);

        if (view->on_load_start)
            view->on_load_start(url, is_redirect);
    }
}

void WebContentClient::did_finish_loading(u64 page_id, URL::URL const& url)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        view->set_url({}, url);

        if (view->on_load_finish)
            view->on_load_finish(url);
    }
}

void WebContentClient::did_finish_text_test(u64 page_id, String const& text)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_text_test_finish)
            view->on_text_test_finish(text);
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

void WebContentClient::did_find_in_page(u64 page_id, size_t current_match_index, Optional<size_t> const& total_match_count)
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

void WebContentClient::did_request_cursor_change(u64 page_id, i32 cursor_type)
{
    if (cursor_type < 0 || cursor_type >= (i32)Gfx::StandardCursor::__Count) {
        dbgln("DidRequestCursorChange: Bad cursor type");
        return;
    }

    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_cursor_change)
            view->on_cursor_change(static_cast<Gfx::StandardCursor>(cursor_type));
    }
}

void WebContentClient::did_change_title(u64 page_id, ByteString const& title)
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

void WebContentClient::did_change_url(u64 page_id, URL::URL const& url)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        view->set_url({}, url);

        if (view->on_url_change)
            view->on_url_change(url);
    }
}

void WebContentClient::did_request_tooltip_override(u64 page_id, Gfx::IntPoint position, ByteString const& title)
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

void WebContentClient::did_enter_tooltip_area(u64 page_id, ByteString const& title)
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

void WebContentClient::did_hover_link(u64 page_id, URL::URL const& url)
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

void WebContentClient::did_click_link(u64 page_id, URL::URL const& url, ByteString const& target, unsigned modifiers)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_link_click)
            view->on_link_click(url, target, modifiers);
    }
}

void WebContentClient::did_middle_click_link(u64 page_id, URL::URL const& url, ByteString const& target, unsigned modifiers)
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

void WebContentClient::did_request_link_context_menu(u64 page_id, Gfx::IntPoint content_position, URL::URL const& url, ByteString const&, unsigned)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_link_context_menu_request)
            view->on_link_context_menu_request(url, view->to_widget_position(content_position));
    }
}

void WebContentClient::did_request_image_context_menu(u64 page_id, Gfx::IntPoint content_position, URL::URL const& url, ByteString const&, unsigned, Optional<Gfx::ShareableBitmap> const& bitmap)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_image_context_menu_request)
            view->on_image_context_menu_request(url, view->to_widget_position(content_position), bitmap);
    }
}

void WebContentClient::did_request_media_context_menu(u64 page_id, Gfx::IntPoint content_position, ByteString const&, unsigned, Web::Page::MediaContextMenu const& menu)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_media_context_menu_request)
            view->on_media_context_menu_request(view->to_widget_position(content_position), menu);
    }
}

void WebContentClient::did_get_source(u64 page_id, URL::URL const& url, URL::URL const& base_url, String const& source)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_received_source)
            view->on_received_source(url, base_url, source);
    }
}

template<typename JsonType = JsonObject>
static JsonType parse_json(StringView json, StringView name)
{
    auto parsed_tree = JsonValue::from_string(json);
    if (parsed_tree.is_error()) {
        dbgln("Unable to parse {}: {}", name, parsed_tree.error());
        return {};
    }

    if constexpr (IsSame<JsonType, JsonObject>) {
        if (!parsed_tree.value().is_object()) {
            dbgln("Expected {} to be an object: {}", name, parsed_tree.value());
            return {};
        }

        return move(parsed_tree.release_value().as_object());
    } else if constexpr (IsSame<JsonType, JsonArray>) {
        if (!parsed_tree.value().is_array()) {
            dbgln("Expected {} to be an array: {}", name, parsed_tree.value());
            return {};
        }

        return move(parsed_tree.release_value().as_array());
    } else {
        static_assert(DependentFalse<JsonType>);
    }
}

void WebContentClient::did_inspect_dom_tree(u64 page_id, String const& dom_tree)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_received_dom_tree)
            view->on_received_dom_tree(parse_json(dom_tree, "DOM tree"sv));
    }
}

void WebContentClient::did_inspect_dom_node(u64 page_id, bool has_style, String const& computed_style, String const& resolved_style, String const& custom_properties, String const& node_box_sizing, String const& aria_properties_state, String const& fonts)
{
    auto view = view_for_page_id(page_id);
    if (!view.has_value() || !view->on_received_dom_node_properties)
        return;

    ViewImplementation::DOMNodeProperties properties;

    if (has_style) {
        properties = ViewImplementation::DOMNodeProperties {
            .computed_style = parse_json(computed_style, "computed style"sv),
            .resolved_style = parse_json(resolved_style, "resolved style"sv),
            .custom_properties = parse_json(custom_properties, "custom properties"sv),
            .node_box_sizing = parse_json(node_box_sizing, "node box sizing"sv),
            .aria_properties_state = parse_json(aria_properties_state, "aria properties state"sv),
            .fonts = parse_json<JsonArray>(fonts, "fonts"sv),
        };
    }

    view->on_received_dom_node_properties(move(properties));
}

void WebContentClient::did_inspect_accessibility_tree(u64 page_id, String const& accessibility_tree)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_received_accessibility_tree)
            view->on_received_accessibility_tree(parse_json(accessibility_tree, "accessibility tree"sv));
    }
}

void WebContentClient::did_get_hovered_node_id(u64 page_id, Web::UniqueNodeID const& node_id)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_received_hovered_node_id)
            view->on_received_hovered_node_id(node_id);
    }
}

void WebContentClient::did_finish_editing_dom_node(u64 page_id, Optional<Web::UniqueNodeID> const& node_id)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_finshed_editing_dom_node)
            view->on_finshed_editing_dom_node(node_id);
    }
}

void WebContentClient::did_get_dom_node_html(u64 page_id, String const& html)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_received_dom_node_html)
            view->on_received_dom_node_html(html);
    }
}

void WebContentClient::did_take_screenshot(u64 page_id, Gfx::ShareableBitmap const& screenshot)
{
    if (auto view = view_for_page_id(page_id); view.has_value())
        view->did_receive_screenshot({}, screenshot);
}

void WebContentClient::did_get_internal_page_info(u64 page_id, WebView::PageInfoType type, String const& info)
{
    if (auto view = view_for_page_id(page_id); view.has_value())
        view->did_receive_internal_page_info({}, type, info);
}

void WebContentClient::did_output_js_console_message(u64 page_id, i32 message_index)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_received_console_message)
            view->on_received_console_message(message_index);
    }
}

void WebContentClient::did_get_js_console_messages(u64 page_id, i32 start_index, Vector<String> const& message_types, Vector<String> const& messages)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_received_console_messages)
            view->on_received_console_messages(start_index, message_types, messages);
    }
}

void WebContentClient::did_request_alert(u64 page_id, String const& message)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_request_alert)
            view->on_request_alert(message);
    }
}

void WebContentClient::did_request_confirm(u64 page_id, String const& message)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_request_confirm)
            view->on_request_confirm(message);
    }
}

void WebContentClient::did_request_prompt(u64 page_id, String const& message, String const& default_)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_request_prompt)
            view->on_request_prompt(message, default_);
    }
}

void WebContentClient::did_request_set_prompt_text(u64 page_id, String const& message)
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

void WebContentClient::did_change_favicon(u64 page_id, Gfx::ShareableBitmap const& favicon)
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

Messages::WebContentClient::DidRequestAllCookiesResponse WebContentClient::did_request_all_cookies(URL::URL const& url)
{
    return Application::cookie_jar().get_all_cookies(url);
}

Messages::WebContentClient::DidRequestNamedCookieResponse WebContentClient::did_request_named_cookie(URL::URL const& url, String const& name)
{
    return Application::cookie_jar().get_named_cookie(url, name);
}

Messages::WebContentClient::DidRequestCookieResponse WebContentClient::did_request_cookie(URL::URL const& url, Web::Cookie::Source source)
{
    return Application::cookie_jar().get_cookie(url, source);
}

void WebContentClient::did_set_cookie(URL::URL const& url, Web::Cookie::ParsedCookie const& cookie, Web::Cookie::Source source)
{
    Application::cookie_jar().set_cookie(url, cookie, source);
}

void WebContentClient::did_update_cookie(Web::Cookie::Cookie const& cookie)
{
    Application::cookie_jar().update_cookie(cookie);
}

void WebContentClient::did_expire_cookies_with_time_offset(AK::Duration offset)
{
    Application::cookie_jar().expire_cookies_with_time_offset(offset);
}

Messages::WebContentClient::DidRequestNewWebViewResponse WebContentClient::did_request_new_web_view(u64 page_id, Web::HTML::ActivateTab const& activate_tab, Web::HTML::WebViewHints const& hints, Optional<u64> const& page_index)
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

void WebContentClient::did_request_file(u64 page_id, ByteString const& path, i32 request_id)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_request_file)
            view->on_request_file(path, request_id);
    }
}

void WebContentClient::did_request_color_picker(u64 page_id, Color const& current_color)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_request_color_picker)
            view->on_request_color_picker(current_color);
    }
}

void WebContentClient::did_request_file_picker(u64 page_id, Web::HTML::FileFilter const& accepted_file_types, Web::HTML::AllowMultipleFiles allow_multiple_files)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_request_file_picker)
            view->on_request_file_picker(accepted_file_types, allow_multiple_files);
    }
}

void WebContentClient::did_request_select_dropdown(u64 page_id, Gfx::IntPoint content_position, i32 minimum_width, Vector<Web::HTML::SelectItem> const& items)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_request_select_dropdown)
            view->on_request_select_dropdown(content_position, minimum_width, items);
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

void WebContentClient::did_insert_clipboard_entry(u64 page_id, String const& data, String const& presentation_style, String const& mime_type)
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

void WebContentClient::did_allocate_backing_stores(u64 page_id, i32 front_bitmap_id, Gfx::ShareableBitmap const& front_bitmap, i32 back_bitmap_id, Gfx::ShareableBitmap const& back_bitmap)
{
    if (auto view = view_for_page_id(page_id); view.has_value())
        view->did_allocate_backing_stores({}, front_bitmap_id, front_bitmap, back_bitmap_id, back_bitmap);
}

void WebContentClient::inspector_did_load(u64 page_id)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_inspector_loaded)
            view->on_inspector_loaded();
    }
}

void WebContentClient::inspector_did_select_dom_node(u64 page_id, Web::UniqueNodeID const& node_id, Optional<Web::CSS::Selector::PseudoElement::Type> const& pseudo_element)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_inspector_selected_dom_node)
            view->on_inspector_selected_dom_node(node_id, pseudo_element);
    }
}

void WebContentClient::inspector_did_set_dom_node_text(u64 page_id, Web::UniqueNodeID const& node_id, String const& text)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_inspector_set_dom_node_text)
            view->on_inspector_set_dom_node_text(node_id, text);
    }
}

void WebContentClient::inspector_did_set_dom_node_tag(u64 page_id, Web::UniqueNodeID const& node_id, String const& tag)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_inspector_set_dom_node_tag)
            view->on_inspector_set_dom_node_tag(node_id, tag);
    }
}

void WebContentClient::inspector_did_add_dom_node_attributes(u64 page_id, Web::UniqueNodeID const& node_id, Vector<Attribute> const& attributes)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_inspector_added_dom_node_attributes)
            view->on_inspector_added_dom_node_attributes(node_id, attributes);
    }
}

void WebContentClient::inspector_did_replace_dom_node_attribute(u64 page_id, Web::UniqueNodeID const& node_id, size_t attribute_index, Vector<Attribute> const& replacement_attributes)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_inspector_replaced_dom_node_attribute)
            view->on_inspector_replaced_dom_node_attribute(node_id, attribute_index, replacement_attributes);
    }
}

void WebContentClient::inspector_did_request_dom_tree_context_menu(u64 page_id, Web::UniqueNodeID const& node_id, Gfx::IntPoint position, String const& type, Optional<String> const& tag, Optional<size_t> const& attribute_index)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_inspector_requested_dom_tree_context_menu)
            view->on_inspector_requested_dom_tree_context_menu(node_id, view->to_widget_position(position), type, tag, attribute_index);
    }
}

void WebContentClient::inspector_did_request_cookie_context_menu(u64 page_id, size_t cookie_index, Gfx::IntPoint position)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_inspector_requested_cookie_context_menu)
            view->on_inspector_requested_cookie_context_menu(cookie_index, view->to_widget_position(position));
    }
}

void WebContentClient::inspector_did_execute_console_script(u64 page_id, String const& script)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_inspector_executed_console_script)
            view->on_inspector_executed_console_script(script);
    }
}

void WebContentClient::inspector_did_export_inspector_html(u64 page_id, String const& html)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_inspector_exported_inspector_html)
            view->on_inspector_exported_inspector_html(html);
    }
}

Messages::WebContentClient::RequestWorkerAgentResponse WebContentClient::request_worker_agent(u64 page_id)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        auto worker_client = MUST(WebView::launch_web_worker_process());
        return worker_client->clone_transport();
    }

    return IPC::File {};
}

void WebContentClient::inspector_did_list_style_sheets(u64 page_id, Vector<Web::CSS::StyleSheetIdentifier> const& stylesheets)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_received_style_sheet_list)
            view->on_received_style_sheet_list(stylesheets);
    }
}

void WebContentClient::inspector_did_request_style_sheet_source(u64 page_id, Web::CSS::StyleSheetIdentifier const& identifier)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_inspector_requested_style_sheet_source)
            view->on_inspector_requested_style_sheet_source(identifier);
    }
}

void WebContentClient::did_get_style_sheet_source(u64 page_id, Web::CSS::StyleSheetIdentifier const& identifier, URL::URL const& base_url, String const& source)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (view->on_received_style_sheet_source)
            view->on_received_style_sheet_source(identifier, base_url, source);
    }
}

Optional<ViewImplementation&> WebContentClient::view_for_page_id(u64 page_id, SourceLocation location)
{
    if (auto view = m_views.get(page_id); view.has_value())
        return *view.value();

    dbgln("WebContentClient::{}: Did not find a page with ID {}", location.function_name(), page_id);
    return {};
}

}
