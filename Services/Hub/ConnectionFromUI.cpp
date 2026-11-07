/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Hub/ActionMirror.h>
#include <Hub/Application.h>
#include <Hub/ConnectionFromUI.h>
#include <Hub/Proxies.h>

namespace Hub {

static ConnectionFromUI* s_the;

ConnectionFromUI::ConnectionFromUI(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionFromClient<HubClientEndpoint, HubServerEndpoint>(*this, move(transport), 1)
{
    s_the = this;

    Application::settings().on_persist = [this](JsonValue const& settings) {
        async_settings_changed(settings.serialized().to_byte_string());
    };
    Application::bookmark_store().on_persist = [this](JsonValue const& bookmarks) {
        async_bookmarks_changed(bookmarks.serialized().to_byte_string());
    };
}

ConnectionFromUI::~ConnectionFromUI()
{
    Application::settings().on_persist = nullptr;
    Application::bookmark_store().on_persist = nullptr;
    s_the = nullptr;
}

ConnectionFromUI* ConnectionFromUI::the()
{
    return s_the;
}

void ConnectionFromUI::die()
{
}

// What the UI knows an input event by, while the Hub has it. The event carries it through the view's queue, in place of
// the data about the event that only the UI can hold. Once the event is gone, the UI can let go of that data.
struct InputFromUI final : public Web::BrowserInputData {
    InputFromUI(u64 view_id, u64 input_id)
        : view_id(view_id)
        , input_id(input_id)
    {
    }

    virtual ~InputFromUI() override
    {
        if (auto* ui = ConnectionFromUI::the())
            ui->async_view_release_input(view_id, input_id);
    }

    u64 view_id { 0 };
    u64 input_id { 0 };
};

u64 ConnectionFromUI::input_id_of(Web::BrowserInputData const& browser_data)
{
    return as<InputFromUI>(browser_data).input_id;
}

template<typename EventType>
static void enqueue_input_from_ui(View& view, u64 input_id, EventType event)
{
    event.browser_data = make<InputFromUI>(view.view_id(), input_id);
    view.enqueue_input_event(move(event));
    view.push_state_to_ui();
}

void ConnectionFromUI::view_enqueue_key_event(u64 view_id, u64 input_id, Web::KeyEvent event)
{
    if (auto* view = find_view(view_id))
        enqueue_input_from_ui(*view, input_id, move(event));
}

void ConnectionFromUI::view_enqueue_mouse_event(u64 view_id, u64 input_id, Web::MouseEvent event)
{
    if (auto* view = find_view(view_id))
        enqueue_input_from_ui(*view, input_id, move(event));
}

void ConnectionFromUI::view_enqueue_drag_event(u64 view_id, u64 input_id, Web::DragEvent event)
{
    if (auto* view = find_view(view_id))
        enqueue_input_from_ui(*view, input_id, move(event));
}

void ConnectionFromUI::view_enqueue_pinch_event(u64 view_id, Web::PinchEvent event)
{
    if (auto* view = find_view(view_id)) {
        view->enqueue_input_event(move(event));
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::release_backing_store(u64 page_id, i32 bitmap_id)
{
    // NB: A page can outlive its view, and a view can stop showing a page, so this goes by the page.
    Web::PageId id { page_id };
    auto client = WebView::WebContentClient::client_for_compositor_context_id(Web::compositor_context_id_for_page(id));
    if (!client.has_value())
        return;
    if (auto* page = client->page(id))
        page->release_presented_bitmap(bitmap_id);
}

void ConnectionFromUI::view_answer(u64 view_id, u64 answer_id, bool answer)
{
    if (auto* view = find_view(view_id)) {
        view->did_get_answer(answer_id, answer);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_answer_permission_request(u64 view_id, u64 answer_id, Web::Permissions::PromptOutcome outcome)
{
    if (auto* view = find_view(view_id)) {
        view->did_get_permission_answer(answer_id, outcome);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::active_view_changed(Optional<u64> view_id)
{
    Application::the().set_active_view(view_id);
}

void ConnectionFromUI::answer_clipboard_text(u64 request_id, Utf16String text)
{
    Application::the().did_read_clipboard_text(request_id, move(text));
}

void ConnectionFromUI::answer_clipboard_item(u64 request_id, Web::Clipboard::SystemClipboardItem item)
{
    Application::the().did_read_clipboard_item(request_id, move(item));
}

void ConnectionFromUI::answer_download_path(u64 request_id, Optional<ByteString> path)
{
    Application::the().did_get_download_path(request_id, move(path));
}

static DownloadState state_of_download(WebView::FileDownloader::Download const& download)
{
    DownloadState state;
    state.id = download.id;
    state.is_private = download.is_private;
    state.url = download.url;
    state.destination = download.destination.string();
    state.status = download.status;
    state.downloaded_size = download.downloaded_size;
    state.total_size = download.total_size;
    state.error = download.error;
    state.can_resume = download.can_resume;
    state.connection_count = download.connection_count;
    state.bytes_per_second = download.bytes_per_second;
    state.is_waiting_to_retry = download.is_waiting_to_retry;
    state.created_time = download.created_time;
    return state;
}

Vector<DownloadState> ConnectionFromUI::downloads()
{
    Vector<DownloadState> downloads;
    for (auto const& download : Application::the().file_downloader().downloads())
        downloads.append(state_of_download(download));
    return downloads;
}

void ConnectionFromUI::download_added(WebView::FileDownloader::Download const& download)
{
    async_download_added(state_of_download(download));
}

void ConnectionFromUI::download_updated(WebView::FileDownloader::Download const& download)
{
    async_download_updated(state_of_download(download));
}

void ConnectionFromUI::download_removed(u64 id, WebView::IsPrivate is_private)
{
    async_download_removed(id, is_private);
}

void ConnectionFromUI::cancel_download(u64 id)
{
    Application::the().file_downloader().cancel_download(id);
}

void ConnectionFromUI::pause_download(u64 id)
{
    Application::the().file_downloader().pause_download(id);
}

void ConnectionFromUI::resume_download(u64 id)
{
    Application::the().file_downloader().resume_download(id);
}

void ConnectionFromUI::show_download_in_folder(u64 id)
{
    auto& application = static_cast<WebView::Application&>(Application::the());
    if (auto download = application.file_downloader().download(id); download.has_value())
        application.show_download_in_folder(*download);
}

void ConnectionFromUI::prune_inactive_downloads(WebView::IsPrivate is_private)
{
    (void)Application::the().file_downloader().prune_inactive_downloads(is_private);
}

void ConnectionFromUI::pause_active_downloads()
{
    Application::the().file_downloader().pause_active_downloads();
}

void ConnectionFromUI::cancel_unresumable_downloads()
{
    Application::the().file_downloader().cancel_unresumable_downloads();
}

void ConnectionFromUI::update_bookmarks(ByteString bookmarks_json)
{
    auto bookmarks = JsonValue::from_string(bookmarks_json);
    if (bookmarks.is_error() || !bookmarks.value().is_object())
        return;
    Application::bookmark_store().replace_with_json(bookmarks.value(), WebView::BookmarkStore::PersistChange::Yes);
}

void ConnectionFromUI::answer_bookmarks_for_all_tabs(u64 request_id, Vector<WebView::BookmarkItem::Bookmark> bookmarks)
{
    Application::the().did_read_bookmarks_for_all_tabs(request_id, move(bookmarks));
}

void ConnectionFromUI::answer_bookmark_dialog(u64 request_id, Optional<Hub::BookmarkDialogResult> result)
{
    Application::the().did_get_bookmark_dialog_result(request_id, move(result));
}

void ConnectionFromUI::bookmark_context_menu_item_changed(Optional<String> item_id, Optional<String> target_folder_id, Optional<String> parent_folder_id)
{
    Application::the().set_bookmark_context_menu_item(move(item_id), move(target_folder_id), move(parent_folder_id));
}

void ConnectionFromUI::omnibox_open(u64 client_id, WebView::IsPrivate is_private)
{
    auto autocomplete = make<WebView::Autocomplete>(is_private);
    autocomplete->on_autocomplete_query_complete = [this, client_id](auto query_id, auto suggestions, auto) {
        async_omnibox_suggestions(client_id, query_id, move(suggestions));
    };
    m_omnibox_clients.set(client_id, move(autocomplete));
}

void ConnectionFromUI::omnibox_close(u64 client_id)
{
    m_omnibox_clients.remove(client_id);
}

void ConnectionFromUI::omnibox_query(u64 client_id, u64 query_id, String query, u64 max_suggestions)
{
    if (auto* autocomplete = m_omnibox_clients.get(client_id).value_or(nullptr))
        autocomplete->query_autocomplete_engine(query_id, move(query), max_suggestions);
}

void ConnectionFromUI::omnibox_cancel(u64 client_id)
{
    if (auto* autocomplete = m_omnibox_clients.get(client_id).value_or(nullptr))
        autocomplete->cancel_pending_query();
}

void ConnectionFromUI::omnibox_record_engagement(u64 client_id, WebView::OmniboxEngagement engagement)
{
    if (auto* autocomplete = m_omnibox_clients.get(client_id).value_or(nullptr))
        autocomplete->record_engagement(move(engagement));
}

void ConnectionFromUI::pip_did_resize(u64 view_id, Gfx::IntSize size)
{
    if (auto* window = PictureInPictureWindowInUI::find(view_id))
        window->did_resize(size);
}

void ConnectionFromUI::pip_did_close(u64 view_id, bool video_keeps_playing)
{
    if (auto* window = PictureInPictureWindowInUI::find(view_id))
        window->did_close(video_keeps_playing);
}

void ConnectionFromUI::notification_shown(u64 id)
{
    if (auto* backend = NotificationBackendInUI::the(); backend && backend->on_shown)
        backend->on_shown(id);
}

void ConnectionFromUI::notification_failed(u64 id)
{
    if (auto* backend = NotificationBackendInUI::the(); backend && backend->on_failed)
        backend->on_failed(id);
}

void ConnectionFromUI::notification_clicked(u64 id)
{
    if (auto* backend = NotificationBackendInUI::the(); backend && backend->on_clicked)
        backend->on_clicked(id);
}

void ConnectionFromUI::notification_closed(u64 id, bool by_user)
{
    if (auto* backend = NotificationBackendInUI::the(); backend && backend->on_closed)
        backend->on_closed(id, by_user ? WebView::NotificationBackend::ByUser::Yes : WebView::NotificationBackend::ByUser::No);
}

void ConnectionFromUI::external_url_handler_resolved(u64 request_id, Optional<u64> handler_id, String application_name, bool is_ladybird)
{
    Application::the().did_resolve_external_url_handler(request_id, handler_id, move(application_name), is_ladybird);
}

void ConnectionFromUI::external_url_launched(u64 request_id, bool launched)
{
    Application::the().did_launch_external_url(request_id, launched);
}

void ConnectionFromUI::update_settings(ByteString settings_json)
{
    auto settings = JsonValue::from_string(settings_json);
    if (settings.is_error() || !settings.value().is_object())
        return;
    Application::settings().replace_with_json(settings.value(), WebView::Settings::PersistChange::Yes);
}

void ConnectionFromUI::activate_action(u64 handle, Optional<bool> checked)
{
    ActionMirror::the().activate(handle, checked);
}

void ConnectionFromUI::destroy_view(u64 view_id)
{
    Application::the().destroy_view(view_id);
}

void ConnectionFromUI::view_set_viewport(u64 view_id, Web::DevicePixelSize size, double device_pixel_ratio)
{
    if (auto* view = find_view(view_id))
        view->set_viewport(size, device_pixel_ratio);
}

void ConnectionFromUI::view_set_system_theme(u64 view_id, Core::AnonymousBuffer theme)
{
    if (auto* view = find_view(view_id))
        view->set_system_theme(move(theme));
}

void ConnectionFromUI::view_set_screen_rects(u64 view_id, WebView::ViewImplementation::ScreenRects screen_rects)
{
    if (auto* view = find_view(view_id))
        view->set_screen_rects(move(screen_rects));
}

void ConnectionFromUI::view_set_page_background_color_to_system_canvas(u64 view_id, bool dark)
{
    if (auto* view = find_view(view_id)) {
        view->set_page_background_color_to_system_canvas(dark);
        view->push_state_to_ui();
    }
}

View* ConnectionFromUI::find_view(u64 view_id)
{
    auto view = WebView::ViewImplementation::find_view_by_id(view_id);
    if (!view.has_value())
        return nullptr;
    return static_cast<View*>(&view.value());
}

void ConnectionFromUI::view_load(u64 view_id, URL::URL url, Web::Bindings::NavigationHistoryBehavior history_behavior)
{
    if (auto* view = find_view(view_id)) {
        view->load(url, history_behavior);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_load_url_from_user_input(u64 view_id, URL::URL url)
{
    if (auto* view = find_view(view_id)) {
        view->load_from_user_input(url);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_load_from_user_input(u64 view_id, ByteString input, Optional<URL::URL> fallback_url)
{
    if (auto* view = find_view(view_id)) {
        view->load_from_user_input(input, move(fallback_url));
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_load_html(u64 view_id, ByteString html)
{
    if (auto* view = find_view(view_id)) {
        view->load_html(html);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_load_navigation_error_page(u64 view_id, ByteString html)
{
    if (auto* view = find_view(view_id)) {
        view->load_navigation_error_page(html);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_find_in_page(u64 view_id, Utf16String query, AK::CaseSensitivity case_sensitivity, bool highlight_all_matches)
{
    if (auto* view = find_view(view_id)) {
        view->find_in_page(query, case_sensitivity, highlight_all_matches);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_set_window_position(u64 view_id, Gfx::IntPoint position)
{
    if (auto* view = find_view(view_id)) {
        view->set_window_position(position);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_set_window_size(u64 view_id, Gfx::IntSize size)
{
    if (auto* view = find_view(view_id)) {
        view->set_window_size(size);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_set_system_visibility_state(u64 view_id, Web::HTML::VisibilityState visibility_state)
{
    if (auto* view = find_view(view_id)) {
        view->set_system_visibility_state(visibility_state);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_set_presents_from_cpu_memory(u64 view_id, bool presents_from_cpu_memory)
{
    if (auto* view = find_view(view_id)) {
        view->set_presents_from_cpu_memory(presents_from_cpu_memory);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_set_has_system_focus(u64 view_id, bool has_system_focus)
{
    if (auto* view = find_view(view_id)) {
        view->set_has_system_focus(has_system_focus);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_open_url_in_new_tab(u64 view_id, URL::URL url, Web::HTML::ActivateTab activate_tab)
{
    if (auto* view = find_view(view_id)) {
        view->open_url_in_new_tab(url, activate_tab);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_open_url_in_new_window(u64 view_id, URL::URL url, WebView::IsPrivate is_private)
{
    if (auto* view = find_view(view_id)) {
        view->open_url_in_new_window(url, is_private);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_set_next_history_visit_transition(u64 view_id, WebView::HistoryVisitTransition transition)
{
    if (auto* view = find_view(view_id)) {
        view->set_next_history_visit_transition(transition);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_reload(u64 view_id, WebView::BypassCache bypass_cache)
{
    if (auto* view = find_view(view_id)) {
        view->reload(bypass_cache);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_stop_loading(u64 view_id)
{
    if (auto* view = find_view(view_id)) {
        view->stop_loading();
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_zoom_in(u64 view_id)
{
    if (auto* view = find_view(view_id)) {
        view->zoom_in();
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_zoom_out(u64 view_id)
{
    if (auto* view = find_view(view_id)) {
        view->zoom_out();
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_reset_zoom(u64 view_id)
{
    if (auto* view = find_view(view_id)) {
        view->reset_zoom();
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_set_display_metadata(u64 view_id, Optional<u64> display_id, double maximum_frames_per_second)
{
    if (auto* view = find_view(view_id)) {
        view->set_display_metadata(display_id, maximum_frames_per_second);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_set_scales_frames_to_fit(u64 view_id, bool scales_frames_to_fit)
{
    if (auto* view = find_view(view_id)) {
        view->set_scales_frames_to_fit(scales_frames_to_fit);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_enable_picture_in_picture_controls(u64 view_id)
{
    if (auto* view = find_view(view_id)) {
        view->enable_picture_in_picture_controls();
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_update_interface_color_scheme(u64 view_id)
{
    if (auto* view = find_view(view_id)) {
        view->update_interface_color_scheme();
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_find_in_page_next_match(u64 view_id)
{
    if (auto* view = find_view(view_id)) {
        view->find_in_page_next_match();
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_find_in_page_previous_match(u64 view_id)
{
    if (auto* view = find_view(view_id)) {
        view->find_in_page_previous_match();
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_find_in_page_end(u64 view_id)
{
    if (auto* view = find_view(view_id)) {
        view->find_in_page_end();
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_report_website_issue(u64 view_id)
{
    if (auto* view = find_view(view_id)) {
        view->report_website_issue();
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_request_accessibility_tree(u64 view_id)
{
    if (auto* view = find_view(view_id)) {
        view->request_accessibility_tree();
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_perform_accessibility_action(u64 view_id, i64 node_id, String action)
{
    if (auto* view = find_view(view_id)) {
        view->perform_accessibility_action(node_id, action);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_clear_node_picker(u64 view_id)
{
    if (auto* view = find_view(view_id)) {
        view->clear_node_picker();
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_node_picker_hover(u64 view_id, Web::DevicePixelPoint position)
{
    if (auto* view = find_view(view_id)) {
        view->node_picker_hover(position);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_node_picker_pick(u64 view_id, Web::DevicePixelPoint position)
{
    if (auto* view = find_view(view_id)) {
        view->node_picker_pick(position);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_node_picker_preview(u64 view_id, Web::DevicePixelPoint position)
{
    if (auto* view = find_view(view_id)) {
        view->node_picker_preview(position);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_node_picker_cancel(u64 view_id)
{
    if (auto* view = find_view(view_id)) {
        view->node_picker_cancel();
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_alert_closed(u64 view_id)
{
    if (auto* view = find_view(view_id)) {
        view->alert_closed();
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_confirm_closed(u64 view_id, bool accepted)
{
    if (auto* view = find_view(view_id)) {
        view->confirm_closed(accepted);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_prompt_closed(u64 view_id, Optional<Utf16String> response)
{
    if (auto* view = find_view(view_id)) {
        view->prompt_closed(response);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_color_picker_update(u64 view_id, Optional<Color> picked_color, Web::HTML::ColorPickerUpdateState state)
{
    if (auto* view = find_view(view_id)) {
        view->color_picker_update(picked_color, state);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_date_picker_closed(u64 view_id, Optional<Utf16String> picked_value)
{
    if (auto* view = find_view(view_id)) {
        view->date_picker_closed(picked_value);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_select_dropdown_closed(u64 view_id, Optional<u32> selected_item_id)
{
    if (auto* view = find_view(view_id)) {
        view->select_dropdown_closed(selected_item_id);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_set_marked_text_from_input_method(u64 view_id, Utf16String text)
{
    if (auto* view = find_view(view_id)) {
        view->set_marked_text_from_input_method(text);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_commit_text_from_input_method(u64 view_id, Utf16String text, i32 replacement_start, i32 replacement_length)
{
    if (auto* view = find_view(view_id)) {
        view->commit_text_from_input_method(text, replacement_start, replacement_length);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_toggle_page_mute_state(u64 view_id)
{
    if (auto* view = find_view(view_id)) {
        view->toggle_page_mute_state();
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_take_and_save_screenshot(u64 view_id, WebView::ViewImplementation::ScreenshotType screenshot_type)
{
    if (auto* view = find_view(view_id)) {
        view->take_and_save_screenshot(screenshot_type);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_request_close(u64 view_id)
{
    if (auto* view = find_view(view_id)) {
        view->request_close();
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_deny_fullscreen_window(u64 view_id)
{
    if (auto* view = find_view(view_id))
        view->deny_fullscreen_window();
}

void ConnectionFromUI::view_exit_fullscreen(u64 view_id)
{
    if (auto* view = find_view(view_id)) {
        view->exit_fullscreen();
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_set_is_fullscreen(u64 view_id, Web::ViewportIsFullscreen is_fullscreen)
{
    if (auto* view = find_view(view_id)) {
        view->set_is_fullscreen(is_fullscreen);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_reset_site_permissions(u64 view_id, URL::Origin origin)
{
    if (auto* view = find_view(view_id)) {
        view->reset_site_permissions(origin);
        view->push_state_to_ui();
    }
}

}
