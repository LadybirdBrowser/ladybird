/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Hub/ActionMirror.h>
#include <Hub/Application.h>
#include <Hub/ConnectionFromUI.h>
#include <LibCore/EventLoop.h>
#include <LibWebView/WebContentPage.h>

namespace Hub {

Application::Application(ApplicationHost& host)
    : m_host(&host)
{
}

View& Application::create_view(View::InitialState initial_state, Optional<WebView::CanonicalTraversable&> traversable)
{
    auto view = make<View>(move(initial_state));
    auto& view_reference = *view;
    m_views.set(view->view_id(), move(view));
    view_reference.initialize(traversable);
    return view_reference;
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
    if (m_host) {
        if (auto authorizer = m_host->create_microphone_authorizer())
            return authorizer.release_nonnull();
    }
    return WebView::Application::create_microphone_authorizer();
}

NonnullOwnPtr<WebView::NotificationBackend> Application::create_notification_backend()
{
    if (m_host) {
        if (auto backend = m_host->create_notification_backend())
            return backend.release_nonnull();
    }
    return WebView::Application::create_notification_backend();
}

void Application::toggle_bookmark_for_view(WebView::ViewImplementation& view)
{
    if (m_host && m_host->show_bookmark_ui_for_view(view))
        return;
    WebView::Application::toggle_bookmark_for_view(view);
}

void Application::rebuild_bookmarks_menu() const
{
    if (!m_host)
        return;
    if (auto* ui = ConnectionFromUI::the())
        ui->async_bookmarks_menu_changed(ActionMirror::the().entries_of(const_cast<Application&>(*this).bookmarks_menu()));
    m_host->rebuild_bookmarks_menu();
}

void Application::on_devtools_enabled() const
{
    WebView::Application::on_devtools_enabled();
    if (m_host)
        m_host->did_enable_devtools();
}

void Application::on_devtools_disabled() const
{
    WebView::Application::on_devtools_disabled();
    if (m_host)
        m_host->did_disable_devtools();
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
    return WebView::Application::ui_font_family();
}

Web::CSS::PreferredColorScheme Application::interface_color_scheme() const
{
    if (m_host)
        return m_host->interface_color_scheme();
    return WebView::Application::interface_color_scheme();
}

Optional<String> Application::system_font_family() const
{
    if (m_host)
        return m_host->system_font_family();
    return WebView::Application::system_font_family();
}

void Application::focus_page_from_notification(WebView::WebContentPage& page)
{
    if (m_host)
        m_host->focus_page_from_notification(page);
    else
        WebView::Application::focus_page_from_notification(page);
}

void Application::open_url_in_new_tab_from_notification(URL::URL const& url, Web::PageId originating_page, WebView::CanonicalTraversable* canonical_traversable, WebView::IsPrivate is_private)
{
    if (m_host)
        m_host->open_url_in_new_tab_from_notification(url, originating_page, canonical_traversable, is_private);
    else
        WebView::Application::open_url_in_new_tab_from_notification(url, originating_page, canonical_traversable, is_private);
}

bool Application::supports_picture_in_picture() const
{
    if (m_host)
        return m_host->supports_picture_in_picture();
    return WebView::Application::supports_picture_in_picture();
}

OwnPtr<WebView::PictureInPictureWindow> Application::create_picture_in_picture_window(WebView::WebContentPage& requesting_page, WebView::CanonicalTraversable& traversable, Gfx::IntSize video_size)
{
    if (!m_host)
        return WebView::Application::create_picture_in_picture_window(requesting_page, traversable, video_size);

    auto& owner_view = static_cast<View&>(requesting_page.view());
    auto initial_state = owner_view.initial_state_for_related_view();
    initial_state.owner_view_id = owner_view.view_id();
    initial_state.is_tab = false;
    auto& view = create_view(move(initial_state), traversable);
    return m_host->create_picture_in_picture_window(owner_view, view.view_id(), video_size);
}

Optional<WebView::ViewImplementation&> Application::active_web_view() const
{
    if (m_host)
        return m_host->active_web_view();
    return WebView::Application::active_web_view();
}

Vector<WebView::BookmarkItem::Bookmark> Application::bookmarks_for_all_tabs_in_current_window() const
{
    if (m_host)
        return m_host->bookmarks_for_all_tabs_in_current_window();
    return {};
}

Optional<WebView::ViewImplementation&> Application::activate_tab_with_url(URL::URL const& url, URL::ExcludeFragment exclude_fragment) const
{
    if (m_host)
        return m_host->activate_tab_with_url(url, exclude_fragment);
    return WebView::Application::activate_tab_with_url(url, exclude_fragment);
}

Optional<WebView::ViewImplementation&> Application::open_blank_new_tab(Web::HTML::ActivateTab activate_tab) const
{
    if (m_host)
        return m_host->open_blank_new_tab(activate_tab);
    return WebView::Application::open_blank_new_tab(activate_tab);
}

void Application::open_url_in_new_tab(URL::URL const& url, Web::HTML::ActivateTab activate_tab) const
{
    if (m_host)
        m_host->open_url_in_new_tab(url, activate_tab);
    else
        WebView::Application::open_url_in_new_tab(url, activate_tab);
}

void Application::open_urls_in_new_tabs(ReadonlySpan<URL::URL> urls) const
{
    if (m_host)
        m_host->open_urls_in_new_tabs(move(urls));
    else
        WebView::Application::open_urls_in_new_tabs(move(urls));
}

void Application::open_url_in_new_window(URL::URL const& url, WebView::IsPrivate is_private)
{
    if (m_host)
        m_host->open_url_in_new_window(url, is_private);
    else
        WebView::Application::open_url_in_new_window(url, is_private);
}

void Application::open_navigation_in_new_tab(Web::HTML::PreparedNavigationDescriptor navigation, Web::HTML::ActivateTab activate_tab) const
{
    if (m_host)
        m_host->open_navigation_in_new_tab(move(navigation), activate_tab);
    else
        WebView::Application::open_navigation_in_new_tab(move(navigation), activate_tab);
}

void Application::open_navigation_in_new_window(Web::HTML::PreparedNavigationDescriptor navigation, WebView::IsPrivate is_private)
{
    if (m_host)
        m_host->open_navigation_in_new_window(move(navigation), is_private);
    else
        WebView::Application::open_navigation_in_new_window(move(navigation), is_private);
}

void Application::resolve_external_url_handler(URL::URL const& url, WebView::ExternalURLHandlerCallback callback) const
{
    if (m_host)
        m_host->resolve_external_url_handler(url, move(callback));
    else
        WebView::Application::resolve_external_url_handler(url, move(callback));
}

NonnullRefPtr<Core::Promise<ByteString>> Application::ask_user_for_download_path(ByteString const& file) const
{
    if (m_host)
        return m_host->ask_user_for_download_path(file);
    return WebView::Application::ask_user_for_download_path(file);
}

void Application::display_download_confirmation_dialog(StringView download_name, LexicalPath const& path) const
{
    if (m_host)
        m_host->display_download_confirmation_dialog(move(download_name), path);
    else
        WebView::Application::display_download_confirmation_dialog(move(download_name), path);
}

void Application::display_error_dialog(StringView error_message) const
{
    if (m_host)
        m_host->display_error_dialog(move(error_message));
    else
        WebView::Application::display_error_dialog(move(error_message));
}

void Application::open_download(WebView::FileDownloader::Download const& download) const
{
    if (m_host)
        m_host->open_download(download);
    else
        WebView::Application::open_download(download);
}

void Application::show_download_in_folder(WebView::FileDownloader::Download const& download) const
{
    if (m_host)
        m_host->show_download_in_folder(download);
    else
        WebView::Application::show_download_in_folder(download);
}

bool Application::supports_clipboard_type(WebView::Application::ClipboardType type) const
{
    if (m_host)
        return m_host->supports_clipboard_type(type);
    return WebView::Application::supports_clipboard_type(type);
}

Utf16String Application::clipboard_text(WebView::Application::ClipboardType type) const
{
    if (m_host)
        return m_host->clipboard_text(type);
    return WebView::Application::clipboard_text(type);
}

void Application::set_clipboard_text(String text, WebView::Application::ClipboardType type)
{
    if (m_host)
        m_host->set_clipboard_text(move(text), type);
    else
        WebView::Application::set_clipboard_text(move(text), type);
}

Web::Clipboard::SystemClipboardItem Application::clipboard_item() const
{
    if (m_host)
        return m_host->clipboard_item();
    return WebView::Application::clipboard_item();
}

void Application::insert_clipboard_item(Web::Clipboard::SystemClipboardItem item)
{
    if (m_host)
        m_host->insert_clipboard_item(move(item));
    else
        WebView::Application::insert_clipboard_item(move(item));
}

bool Application::supports_system_menu_bar() const
{
    if (m_host)
        return m_host->supports_system_menu_bar();
    return WebView::Application::supports_system_menu_bar();
}

bool Application::supports_vertical_tabs() const
{
    if (m_host)
        return m_host->supports_vertical_tabs();
    return WebView::Application::supports_vertical_tabs();
}

bool Application::supports_private_browsing_windows() const
{
    if (m_host)
        return m_host->supports_private_browsing_windows();
    return WebView::Application::supports_private_browsing_windows();
}

bool Application::supports_client_side_window_decorations() const
{
    if (m_host)
        return m_host->supports_client_side_window_decorations();
    return WebView::Application::supports_client_side_window_decorations();
}

bool Application::platform_reports_scroll_momentum() const
{
    if (m_host)
        return m_host->platform_reports_scroll_momentum();
    return WebView::Application::platform_reports_scroll_momentum();
}

bool Application::platform_scrolls_elastically() const
{
    if (m_host)
        return m_host->platform_scrolls_elastically();
    return WebView::Application::platform_scrolls_elastically();
}

bool Application::platform_scrollbar_track_press_jumps() const
{
    if (m_host)
        return m_host->platform_scrollbar_track_press_jumps();
    return WebView::Application::platform_scrollbar_track_press_jumps();
}

Vector<String> Application::platform_preferred_languages() const
{
    if (m_host)
        return m_host->platform_preferred_languages();
    return WebView::Application::platform_preferred_languages();
}

Optional<Gfx::GpuDeviceIdentity> Application::platform_presentation_gpu_device() const
{
    if (m_host)
        return m_host->platform_presentation_gpu_device();
    return WebView::Application::platform_presentation_gpu_device();
}

Vector<u64> Application::platform_presentation_drm_format_modifiers() const
{
    if (m_host)
        return m_host->platform_presentation_drm_format_modifiers();
    return WebView::Application::platform_presentation_drm_format_modifiers();
}

void Application::update_tabs_display() const
{
    if (m_host)
        m_host->update_tabs_display();
    else
        WebView::Application::update_tabs_display();
}

void Application::show_bookmark_context_menu(Gfx::IntPoint position, Optional<WebView::BookmarkItem const&> item, Optional<String const&> target_folder_id, Optional<String const&> parent_folder_id)
{
    if (m_host)
        m_host->show_bookmark_context_menu(position, move(item), move(target_folder_id), move(parent_folder_id));
    else
        WebView::Application::show_bookmark_context_menu(position, move(item), move(target_folder_id), move(parent_folder_id));
}

Optional<WebView::Application::BookmarkID> Application::bookmark_item_id_for_context_menu() const
{
    if (m_host)
        return m_host->bookmark_item_id_for_context_menu();
    return WebView::Application::bookmark_item_id_for_context_menu();
}

NonnullRefPtr<WebView::Application::BookmarkPromise> Application::display_add_bookmark_dialog(Optional<String const&> target_folder_id) const
{
    if (m_host)
        return m_host->display_add_bookmark_dialog(move(target_folder_id));
    return WebView::Application::display_add_bookmark_dialog(move(target_folder_id));
}

NonnullRefPtr<WebView::Application::BookmarkPromise> Application::display_edit_bookmark_dialog(WebView::BookmarkItem const& current_bookmark, Optional<String const&> parent_folder_id) const
{
    if (m_host)
        return m_host->display_edit_bookmark_dialog(current_bookmark, move(parent_folder_id));
    return WebView::Application::display_edit_bookmark_dialog(current_bookmark, move(parent_folder_id));
}

NonnullRefPtr<WebView::Application::BookmarkPromise> Application::display_add_bookmark_folder_dialog(Optional<String const&> default_title, Optional<String const&> target_folder_id) const
{
    if (m_host)
        return m_host->display_add_bookmark_folder_dialog(move(default_title), move(target_folder_id));
    return WebView::Application::display_add_bookmark_folder_dialog(move(default_title), move(target_folder_id));
}

NonnullRefPtr<WebView::Application::BookmarkPromise> Application::display_edit_bookmark_folder_dialog(WebView::BookmarkItem const& current_folder, Optional<String const&> parent_folder_id) const
{
    if (m_host)
        return m_host->display_edit_bookmark_folder_dialog(current_folder, move(parent_folder_id));
    return WebView::Application::display_edit_bookmark_folder_dialog(current_folder, move(parent_folder_id));
}

void Application::on_recently_closed_entries_changed() const
{
    if (m_host)
        m_host->on_recently_closed_entries_changed();
    else
        WebView::Application::on_recently_closed_entries_changed();
}

}
