/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Hex.h>
#include <AK/MemoryStream.h>
#include <AK/Queue.h>
#include <Hub/ActionMirror.h>
#include <Hub/Application.h>
#include <Hub/ConnectionFromUI.h>
#include <Hub/Proxies.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/EventLoop.h>
#include <LibIPC/Decoder.h>
#include <LibWebView/WebContentPage.h>

#if defined(AK_OS_MACOS)
#    include <LibWebView/MicrophoneAuthorizerMacOS.h>
#endif

namespace Hub {

Application::Application(ApplicationHost* host)
    : m_host(host)
{
}

void Application::create_platform_arguments(Core::ArgsParser& args_parser)
{
    args_parser.add_option(m_encoded_ui_info, "What the UI tells the Hub to start with", "ui-info", 0, "hex");
    args_parser.add_option(m_ui_mach_server_name, "Mach server to get the connection to the UI from", "ui-mach-server", 0, "name");
}

Hub::UIInfo const* Application::ui_info() const
{
    if (!m_ui_info.has_value() && !m_encoded_ui_info.is_empty()) {
        auto bytes = decode_hex(m_encoded_ui_info);
        if (bytes.is_error())
            return nullptr;
        FixedMemoryStream stream { bytes.value().bytes() };
        Queue<IPC::Attachment> attachments;
        IPC::Decoder decoder { stream, attachments };
        auto info = decoder.decode<Hub::UIInfo>();
        if (info.is_error())
            return nullptr;
        m_ui_info = info.release_value();
    }
    return m_ui_info.has_value() ? &*m_ui_info : nullptr;
}

void Application::set_ui_info(Hub::UIInfo info)
{
    auto scrollbar_track_press_jumps_changed = !m_ui_info.has_value() || m_ui_info->scrollbar_track_press_jumps != info.scrollbar_track_press_jumps;
    m_ui_info = move(info);
    if (scrollbar_track_press_jumps_changed)
        platform_scrollbar_track_press_behavior_changed();
}

View& Application::create_view(View::InitialState initial_state, Optional<WebView::CanonicalTraversable&> traversable)
{
    auto view = make<View>(move(initial_state));
    auto& view_reference = *view;
    m_views.set(view->view_id(), move(view));
    view_reference.initialize(traversable);
    return view_reference;
}

void Application::notify_webdriver_window_created(String const& handle)
{
    // NB: WebDriver drives a window from the moment it hears of it. So, it must not hear of one before the UI has asked for
    //     what its windows start out showing, or that would replace the page WebDriver went to.
    if (ConnectionFromUI::the() && !m_ui_did_start) {
        m_webdriver_windows_awaiting_ui.append(handle);
        return;
    }
    WebView::Application::notify_webdriver_window_created(handle);
}

void Application::ui_did_start()
{
    m_ui_did_start = true;
    for (auto const& handle : m_webdriver_windows_awaiting_ui)
        WebView::Application::notify_webdriver_window_created(handle);
    m_webdriver_windows_awaiting_ui.clear();
}

void Application::set_active_view(Optional<u64> view_id)
{
    m_active_view_id = view_id;
    update_actions_for_active_web_view();
}

Optional<WebView::ViewImplementation&> Application::active_web_view() const
{
    if (!m_active_view_id.has_value())
        return {};
    return WebView::ViewImplementation::find_view_by_id(*m_active_view_id);
}

Optional<WebView::ViewImplementation&> Application::open_blank_new_tab(Web::HTML::ActivateTab activate_tab, WebView::ViewImplementation const* related_view) const
{
    auto* ui = ConnectionFromUI::the();
    if (!ui)
        return WebView::Application::open_blank_new_tab(activate_tab, related_view);

    // NB: A tab opened for a page starts out like that page's view, with its privacy, whichever view is active by now.
    View::InitialState initial_state;
    if (related_view)
        initial_state = static_cast<View const&>(*related_view).initial_state_for_related_view();
    else if (auto active_view = active_web_view(); active_view.has_value())
        initial_state = static_cast<View&>(*active_view).initial_state_for_related_view();
    auto& view = const_cast<Application&>(*this).create_view(move(initial_state));
    Optional<u64> source_view_id;
    if (related_view)
        source_view_id = related_view->view_id();
    ui->async_app_open_view_in_new_tab(view.view_id(), source_view_id, view.is_private(), activate_tab);
    return view;
}

// NB: Which window a tab goes to must not depend on what is active once the UI reads the message. A tab must stay in
//     the browsing session of the page that opened it. With no such page, the active view here is the one that was
//     active when the UI sent what led to this, since the UI's messages arrive in order.
Application::NewTabSource Application::new_tab_source(WebView::ViewImplementation const* source_view) const
{
    auto active_view = active_web_view();
    if (!source_view && active_view.has_value())
        source_view = &*active_view;
    if (!source_view)
        return {};
    return { source_view->view_id(), source_view->is_private() };
}

void Application::open_url_in_new_tab(URL::URL const& url, Web::HTML::ActivateTab activate_tab, WebView::ViewImplementation const* source_view) const
{
    if (auto* ui = ConnectionFromUI::the()) {
        auto source = new_tab_source(source_view);
        ui->async_app_open_url_in_new_tab(url, activate_tab, source.view_id, source.is_private);
    } else {
        WebView::Application::open_url_in_new_tab(url, activate_tab, source_view);
    }
}

void Application::activate_or_open_tab_with_url(URL::URL const& url) const
{
    if (auto* ui = ConnectionFromUI::the())
        ui->async_app_activate_or_open_tab_with_url(url);
    else
        WebView::Application::activate_or_open_tab_with_url(url);
}

void Application::open_urls_in_new_tabs(ReadonlySpan<URL::URL> urls) const
{
    auto* ui = ConnectionFromUI::the();
    if (!ui) {
        WebView::Application::open_urls_in_new_tabs(urls);
        return;
    }
    Vector<URL::URL> urls_to_open;
    urls_to_open.append(urls.data(), urls.size());
    ui->async_app_open_urls_in_new_tabs(move(urls_to_open));
}

void Application::open_url_in_new_window(URL::URL const& url, WebView::IsPrivate is_private)
{
    if (auto* ui = ConnectionFromUI::the())
        ui->async_app_open_url_in_new_window(url, is_private);
}

void Application::open_navigation_in_new_tab(Web::HTML::PreparedNavigationDescriptor navigation, Web::HTML::ActivateTab activate_tab, WebView::ViewImplementation const* source_view) const
{
    if (auto* ui = ConnectionFromUI::the()) {
        auto source = new_tab_source(source_view);
        ui->async_app_open_navigation_in_new_tab(move(navigation), activate_tab, source.view_id, source.is_private);
    } else {
        WebView::Application::open_navigation_in_new_tab(move(navigation), activate_tab, source_view);
    }
}

void Application::open_navigation_in_new_window(Web::HTML::PreparedNavigationDescriptor navigation, WebView::IsPrivate is_private)
{
    if (auto* ui = ConnectionFromUI::the())
        ui->async_app_open_navigation_in_new_window(move(navigation), is_private);
}

NonnullRefPtr<Core::Promise<ByteString>> Application::ask_user_for_download_path(ByteString const& file) const
{
    auto* ui = ConnectionFromUI::the();
    if (!ui)
        return WebView::Application::ask_user_for_download_path(file);

    auto promise = Core::Promise<ByteString>::construct();
    auto request_id = m_next_ui_request_id++;
    m_pending_download_paths.set(request_id, promise);
    ui->async_app_ask_user_for_download_path(request_id, file);
    return promise;
}

void Application::did_get_download_path(u64 request_id, Optional<ByteString> path)
{
    auto promise = m_pending_download_paths.take(request_id);
    if (!promise.has_value())
        return;
    if (path.has_value())
        (*promise)->resolve(path.release_value());
    else
        (*promise)->reject(Error::from_errno(ECANCELED));
}

void Application::display_download_confirmation_dialog(StringView download_name, LexicalPath const& path) const
{
    if (auto* ui = ConnectionFromUI::the())
        ui->async_app_display_download_confirmation_dialog(MUST(String::from_utf8(download_name)), path.string());
    else
        WebView::Application::display_download_confirmation_dialog(download_name, path);
}

void Application::display_error_dialog(StringView error_message) const
{
    if (auto* ui = ConnectionFromUI::the())
        ui->async_app_display_error_dialog(MUST(String::from_utf8(error_message)));
    else
        WebView::Application::display_error_dialog(error_message);
}

void Application::open_download(WebView::FileDownloader::Download const& download) const
{
    auto path = download_file_path_for_frontend_action(download);
    if (path.is_error()) {
        display_error_dialog("Unable to open downloaded file: path cannot be represented by this frontend"sv);
        return;
    }
    if (auto* ui = ConnectionFromUI::the())
        ui->async_app_open_file(path.release_value());
}

void Application::show_download_in_folder(WebView::FileDownloader::Download const& download) const
{
    auto path = download_directory_path_for_frontend_action(download);
    if (path.is_error()) {
        display_error_dialog("Unable to show downloaded file: path cannot be represented by this frontend"sv);
        return;
    }
    if (auto* ui = ConnectionFromUI::the())
        ui->async_app_open_file(path.release_value());
}

void Application::set_clipboard_text(String text, ClipboardType type)
{
    if (auto* ui = ConnectionFromUI::the())
        ui->async_app_set_clipboard_text(move(text), type);
    else
        WebView::Application::set_clipboard_text(move(text), type);
}

void Application::insert_clipboard_item(Web::Clipboard::SystemClipboardItem item)
{
    if (auto* ui = ConnectionFromUI::the())
        ui->async_app_insert_clipboard_item(move(item));
    else
        WebView::Application::insert_clipboard_item(move(item));
}

void Application::read_clipboard_text(ClipboardType type, Function<void(Utf16String)> on_read) const
{
    auto* ui = ConnectionFromUI::the();
    if (!ui) {
        WebView::Application::read_clipboard_text(type, move(on_read));
        return;
    }
    auto request_id = m_next_ui_request_id++;
    m_pending_clipboard_text_reads.set(request_id, move(on_read));
    ui->async_app_read_clipboard_text(request_id, type);
}

void Application::did_read_clipboard_text(u64 request_id, Utf16String text)
{
    if (auto on_read = m_pending_clipboard_text_reads.take(request_id); on_read.has_value())
        (*on_read)(move(text));
}

void Application::read_clipboard_item(Function<void(Web::Clipboard::SystemClipboardItem)> on_read) const
{
    auto* ui = ConnectionFromUI::the();
    if (!ui) {
        WebView::Application::read_clipboard_item(move(on_read));
        return;
    }
    auto request_id = m_next_ui_request_id++;
    m_pending_clipboard_item_reads.set(request_id, move(on_read));
    ui->async_app_read_clipboard_item(request_id);
}

void Application::did_read_clipboard_item(u64 request_id, Web::Clipboard::SystemClipboardItem item)
{
    if (auto on_read = m_pending_clipboard_item_reads.take(request_id); on_read.has_value())
        (*on_read)(move(item));
}

void Application::update_tabs_display() const
{
    if (auto* ui = ConnectionFromUI::the())
        ui->async_app_update_tabs_display();
}

void Application::on_recently_closed_entries_changed() const
{
    if (auto* ui = ConnectionFromUI::the())
        ui->async_app_on_recently_closed_entries_changed();
}

void Application::destroy_view(u64 view_id)
{
    m_views.remove(view_id);
}

Core::EventLoop& Application::create_platform_event_loop()
{
    // Headless mode has no UI to show anything in.
    if (browser_options().headless_mode.has_value())
        detach_from_host();

    if (m_host)
        m_host->will_create_event_loop(arguments());
    auto& event_loop = WebView::Application::create_platform_event_loop();
    if (m_host)
        m_host->did_create_event_loop(event_loop);
    return event_loop;
}

NonnullOwnPtr<WebView::MicrophoneAuthorizer> Application::create_microphone_authorizer()
{
#if defined(AK_OS_MACOS)
    if (!browser_options().headless_mode.has_value())
        return make<WebView::MicrophoneAuthorizerMacOS>();
#endif
    return WebView::Application::create_microphone_authorizer();
}

NonnullOwnPtr<WebView::NotificationBackend> Application::create_notification_backend()
{
    if (ConnectionFromUI::the())
        return make<NotificationBackendInUI>();
    return WebView::Application::create_notification_backend();
}

void Application::toggle_bookmark_for_view(WebView::ViewImplementation& view)
{
    // The bookmark popover of the active tab asks the user about the bookmark.
    if (auto* ui = ConnectionFromUI::the(); ui && m_active_view_id == view.view_id()) {
        ui->async_app_show_bookmark_popover();
        return;
    }
    WebView::Application::toggle_bookmark_for_view(view);
}

void Application::read_bookmarks_for_all_tabs_in_current_window(Function<void(Vector<WebView::BookmarkItem::Bookmark>)> on_read) const
{
    auto* ui = ConnectionFromUI::the();
    if (!ui || !m_active_view_id.has_value()) {
        on_read({});
        return;
    }
    auto request_id = m_next_ui_request_id++;
    m_pending_bookmarks_for_all_tabs.set(request_id, move(on_read));
    ui->async_app_read_bookmarks_for_all_tabs(request_id, *m_active_view_id);
}

void Application::did_read_bookmarks_for_all_tabs(u64 request_id, Vector<WebView::BookmarkItem::Bookmark> bookmarks)
{
    if (auto on_read = m_pending_bookmarks_for_all_tabs.take(request_id); on_read.has_value())
        (*on_read)(move(bookmarks));
}

void Application::show_bookmark_context_menu(Gfx::IntPoint position, Optional<WebView::BookmarkItem const&> item, Optional<String const&> target_folder_id, Optional<String const&> parent_folder_id)
{
    if (auto* ui = ConnectionFromUI::the())
        ui->async_app_show_bookmark_context_menu(position, item.map([](auto const& item) { return item.id; }), target_folder_id.copy(), parent_folder_id.copy());
}

void Application::set_bookmark_context_menu_item(Optional<String> item_id, Optional<String> target_folder_id, Optional<String> parent_folder_id)
{
    if (!item_id.has_value() && !target_folder_id.has_value()) {
        m_bookmark_context_menu_item = {};
        return;
    }
    m_bookmark_context_menu_item = BookmarkID {
        .id = item_id.value_or({}),
        .target_folder_id = move(target_folder_id),
        .parent_folder_id = move(parent_folder_id),
    };
}

NonnullRefPtr<Application::BookmarkPromise> Application::display_bookmark_dialog(BookmarkDialogKind kind, Optional<String> item_id, Optional<String> default_title, Optional<String> folder_id) const
{
    auto promise = BookmarkPromise::construct();
    auto* ui = ConnectionFromUI::the();
    if (!ui) {
        promise->reject(Error::from_errno(ECANCELED));
        return promise;
    }
    auto request_id = m_next_ui_request_id++;
    m_pending_bookmark_dialogs.set(request_id, promise);
    ui->async_app_display_bookmark_dialog(request_id, kind, move(item_id), move(default_title), move(folder_id));
    return promise;
}

void Application::did_get_bookmark_dialog_result(u64 request_id, Optional<Hub::BookmarkDialogResult> result)
{
    auto promise = m_pending_bookmark_dialogs.take(request_id);
    if (!promise.has_value())
        return;
    if (!result.has_value()) {
        (*promise)->reject(Error::from_errno(ECANCELED));
        return;
    }
    if (result->is_folder) {
        (*promise)->resolve({ .data = WebView::BookmarkItem::Folder { .title = move(result->title), .children = {} }, .target_folder_id = move(result->target_folder_id) });
        return;
    }
    if (!result->url.has_value()) {
        (*promise)->reject(Error::from_errno(ECANCELED));
        return;
    }
    (*promise)->resolve({ .data = WebView::BookmarkItem::Bookmark { .url = result->url.release_value(), .title = move(result->title), .favicon_hash = move(result->favicon_hash) }, .target_folder_id = move(result->target_folder_id) });
}

NonnullRefPtr<Application::BookmarkPromise> Application::display_add_bookmark_dialog(Optional<String const&> target_folder_id) const
{
    return display_bookmark_dialog(BookmarkDialogKind::AddBookmark, {}, {}, target_folder_id.copy());
}

NonnullRefPtr<Application::BookmarkPromise> Application::display_edit_bookmark_dialog(WebView::BookmarkItem const& current_bookmark, Optional<String const&> parent_folder_id) const
{
    return display_bookmark_dialog(BookmarkDialogKind::EditBookmark, current_bookmark.id, {}, parent_folder_id.copy());
}

NonnullRefPtr<Application::BookmarkPromise> Application::display_add_bookmark_folder_dialog(Optional<String const&> default_title, Optional<String const&> target_folder_id) const
{
    return display_bookmark_dialog(BookmarkDialogKind::AddFolder, {}, default_title.copy(), target_folder_id.copy());
}

NonnullRefPtr<Application::BookmarkPromise> Application::display_edit_bookmark_folder_dialog(WebView::BookmarkItem const& current_folder, Optional<String const&> parent_folder_id) const
{
    return display_bookmark_dialog(BookmarkDialogKind::EditFolder, current_folder.id, {}, parent_folder_id.copy());
}

void Application::rebuild_bookmarks_menu() const
{
    if (auto* ui = ConnectionFromUI::the())
        ui->async_bookmarks_menu_changed(ActionMirror::the().entries_of(const_cast<Application&>(*this).bookmarks_menu()));
}

void Application::on_devtools_enabled() const
{
    WebView::Application::on_devtools_enabled();
    if (auto* ui = ConnectionFromUI::the())
        ui->async_app_did_enable_devtools();
}

void Application::on_devtools_disabled() const
{
    WebView::Application::on_devtools_disabled();
    if (auto* ui = ConnectionFromUI::the())
        ui->async_app_did_disable_devtools();
}

WebView::SessionLaunchPolicy Application::launch_restore_policy() const
{
    // Without the setting, only pinned tabs come back; with it, every window and tab does.
    if (settings().restore_previous_session())
        return WebView::SessionLaunchPolicy::Everything;
    return WebView::SessionLaunchPolicy::PinnedOnly;
}

void Application::create_platform_actions()
{
    if (m_host)
        m_host->create_platform_actions();
    else
        WebView::Application::create_platform_actions();
}

Optional<String> Application::ui_font_family() const
{
    if (m_host)
        return m_host->ui_font_family();
    if (auto const* info = ui_info())
        return info->ui_font_family;
    return WebView::Application::ui_font_family();
}

Web::CSS::PreferredColorScheme Application::interface_color_scheme() const
{
    if (m_host)
        return m_host->interface_color_scheme();
    if (auto const* info = ui_info())
        return info->interface_color_scheme;
    return WebView::Application::interface_color_scheme();
}

Optional<String> Application::system_font_family() const
{
    if (m_host)
        return m_host->system_font_family();
    if (auto const* info = ui_info())
        return info->system_font_family;
    return WebView::Application::system_font_family();
}

bool Application::supports_picture_in_picture() const
{
    if (m_host)
        return m_host->supports_picture_in_picture();
    if (auto const* info = ui_info())
        return info->supports_picture_in_picture;
    return WebView::Application::supports_picture_in_picture();
}

OwnPtr<WebView::PictureInPictureWindow> Application::create_picture_in_picture_window(WebView::WebContentPage& requesting_page, WebView::CanonicalTraversable& traversable, Gfx::IntSize video_size)
{
    if (!ConnectionFromUI::the())
        return WebView::Application::create_picture_in_picture_window(requesting_page, traversable, video_size);

    auto& owner_view = static_cast<View&>(requesting_page.view());
    auto initial_state = owner_view.initial_state_for_related_view();
    initial_state.owner_view_id = owner_view.view_id();
    initial_state.is_tab = false;
    auto screen_size = owner_view.screen_size();
    auto& view = create_view(move(initial_state), traversable);
    return make<PictureInPictureWindowInUI>(view, owner_view.view_id(), video_size, WebView::PictureInPictureWindow::initial_size(video_size, screen_size));
}

void Application::focus_page_from_notification(WebView::WebContentPage& page)
{
    if (auto* ui = ConnectionFromUI::the())
        ui->async_app_focus_view_from_notification(page.view().view_id());
}

void Application::open_url_in_new_tab_from_notification(URL::URL const& url, Web::PageId, WebView::CanonicalTraversable* canonical_traversable, WebView::IsPrivate is_private)
{
    Optional<u64> view_id;
    if (canonical_traversable) {
        if (auto view = canonical_traversable->view(); view.has_value())
            view_id = view->view_id();
    }
    if (auto* ui = ConnectionFromUI::the())
        ui->async_app_open_url_in_new_tab_from_notification(url, view_id, is_private);
}

void Application::resolve_external_url_handler(URL::URL const& url, WebView::ExternalURLHandlerCallback callback) const
{
    auto* ui = ConnectionFromUI::the();
    if (!ui) {
        WebView::Application::resolve_external_url_handler(url, move(callback));
        return;
    }
    auto request_id = m_next_ui_request_id++;
    m_pending_external_url_handlers.set(request_id, move(callback));
    ui->async_external_url_resolve_handler(request_id, url);
}

void Application::did_resolve_external_url_handler(u64 request_id, Optional<u64> handler_id, String application_name, bool is_ladybird)
{
    auto callback = m_pending_external_url_handlers.take(request_id);
    if (!callback.has_value())
        return;
    if (handler_id.has_value())
        (*callback)(adopt_ref(*new ExternalURLHandlerInUI(*handler_id, move(application_name), is_ladybird)));
    else
        (*callback)(nullptr);
}

void Application::launch_external_url(u64 handler_id, URL::URL const& url, Function<void(bool)> callback) const
{
    auto* ui = ConnectionFromUI::the();
    if (!ui) {
        callback(false);
        return;
    }
    auto request_id = m_next_ui_request_id++;
    m_pending_external_url_launches.set(request_id, move(callback));
    ui->async_external_url_launch(handler_id, request_id, url);
}

void Application::did_launch_external_url(u64 request_id, bool launched)
{
    if (auto callback = m_pending_external_url_launches.take(request_id); callback.has_value())
        (*callback)(launched);
}

bool Application::supports_clipboard_type(WebView::Application::ClipboardType type) const
{
    if (m_host)
        return m_host->supports_clipboard_type(type);
    if (auto const* info = ui_info())
        return type == ClipboardType::Text || info->supports_selection_clipboard;
    return WebView::Application::supports_clipboard_type(type);
}

bool Application::supports_system_menu_bar() const
{
    if (m_host)
        return m_host->supports_system_menu_bar();
    if (auto const* info = ui_info())
        return info->supports_system_menu_bar;
    return WebView::Application::supports_system_menu_bar();
}

bool Application::supports_vertical_tabs() const
{
    if (m_host)
        return m_host->supports_vertical_tabs();
    if (auto const* info = ui_info())
        return info->supports_vertical_tabs;
    return WebView::Application::supports_vertical_tabs();
}

bool Application::supports_private_browsing_windows() const
{
    if (m_host)
        return m_host->supports_private_browsing_windows();
    if (auto const* info = ui_info())
        return info->supports_private_browsing_windows;
    return WebView::Application::supports_private_browsing_windows();
}

bool Application::supports_client_side_window_decorations() const
{
    if (m_host)
        return m_host->supports_client_side_window_decorations();
    if (auto const* info = ui_info())
        return info->supports_client_side_window_decorations;
    return WebView::Application::supports_client_side_window_decorations();
}

bool Application::platform_reports_scroll_momentum() const
{
    if (m_host)
        return m_host->platform_reports_scroll_momentum();
    if (auto const* info = ui_info())
        return info->reports_scroll_momentum;
    return WebView::Application::platform_reports_scroll_momentum();
}

bool Application::platform_scrolls_elastically() const
{
    if (m_host)
        return m_host->platform_scrolls_elastically();
    if (auto const* info = ui_info())
        return info->scrolls_elastically;
    return WebView::Application::platform_scrolls_elastically();
}

bool Application::platform_scrollbar_track_press_jumps() const
{
    if (m_host)
        return m_host->platform_scrollbar_track_press_jumps();
    if (auto const* info = ui_info())
        return info->scrollbar_track_press_jumps;
    return WebView::Application::platform_scrollbar_track_press_jumps();
}

Vector<String> Application::platform_preferred_languages() const
{
    if (m_host)
        return m_host->platform_preferred_languages();
    if (auto const* info = ui_info())
        return info->preferred_languages;
    return WebView::Application::platform_preferred_languages();
}

Optional<Gfx::GpuDeviceIdentity> Application::platform_presentation_gpu_device() const
{
    if (m_host)
        return m_host->platform_presentation_gpu_device();
    if (auto const* info = ui_info())
        return info->presentation_gpu_device;
    return WebView::Application::platform_presentation_gpu_device();
}

Vector<u64> Application::platform_presentation_drm_format_modifiers() const
{
    if (m_host)
        return m_host->platform_presentation_drm_format_modifiers();
    if (auto const* info = ui_info())
        return info->presentation_drm_format_modifiers;
    return WebView::Application::platform_presentation_drm_format_modifiers();
}

}
