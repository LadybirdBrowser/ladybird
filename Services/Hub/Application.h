/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <Hub/View.h>
#include <LibCore/Promise.h>
#include <LibMain/Main.h>
#include <LibWebView/Application.h>
#include <LibWebView/MicrophoneAuthorizer.h>
#include <LibWebView/NotificationBackend.h>
#include <LibWebView/PictureInPictureWindow.h>

namespace Hub {

// What the application needs from the UI. With no UI, as in headless mode, it does without.
class ApplicationHost {
public:
    virtual ~ApplicationHost() = default;

    virtual void will_create_event_loop(Main::Arguments&) = 0;
    virtual void did_create_event_loop(Core::EventLoop&) = 0;

    // The platform's own, if it has one.
    virtual OwnPtr<WebView::MicrophoneAuthorizer> create_microphone_authorizer() = 0;
    virtual OwnPtr<WebView::NotificationBackend> create_notification_backend() = 0;

    // Returns whether the UI took care of it, by asking the user.
    virtual bool show_bookmark_ui_for_view(WebView::ViewImplementation&) = 0;

    virtual void did_enable_devtools() const = 0;
    virtual void did_disable_devtools() const = 0;

    virtual void create_platform_actions() = 0;
    virtual Optional<String> ui_font_family() const = 0;
    virtual Web::CSS::PreferredColorScheme interface_color_scheme() const = 0;
    virtual Optional<String> system_font_family() const = 0;
    virtual void focus_page_from_notification(WebView::WebContentPage& page) = 0;
    virtual void open_url_in_new_tab_from_notification(URL::URL const& url, Web::PageId originating_page, WebView::CanonicalTraversable* canonical_traversable, WebView::IsPrivate is_private) = 0;
    virtual bool supports_picture_in_picture() const = 0;
    virtual OwnPtr<WebView::PictureInPictureWindow> create_picture_in_picture_window(WebView::ViewImplementation& owner_view, u64 view_id, Gfx::IntSize video_size) = 0;
    virtual Optional<WebView::ViewImplementation&> active_web_view() const = 0;
    virtual Vector<WebView::BookmarkItem::Bookmark> bookmarks_for_all_tabs_in_current_window() const = 0;
    virtual Optional<WebView::ViewImplementation&> activate_tab_with_url(URL::URL const& url, URL::ExcludeFragment exclude_fragment) const = 0;
    virtual Optional<WebView::ViewImplementation&> open_blank_new_tab(Web::HTML::ActivateTab activate_tab) const = 0;
    virtual void open_url_in_new_tab(URL::URL const& url, Web::HTML::ActivateTab activate_tab) const = 0;
    virtual void open_urls_in_new_tabs(ReadonlySpan<URL::URL> urls) const = 0;
    virtual void open_url_in_new_window(URL::URL const& url, WebView::IsPrivate is_private) = 0;
    virtual void open_navigation_in_new_tab(Web::HTML::PreparedNavigationDescriptor navigation, Web::HTML::ActivateTab activate_tab) const = 0;
    virtual void open_navigation_in_new_window(Web::HTML::PreparedNavigationDescriptor navigation, WebView::IsPrivate is_private) = 0;
    virtual void resolve_external_url_handler(URL::URL const& url, WebView::ExternalURLHandlerCallback callback) const = 0;
    virtual NonnullRefPtr<Core::Promise<ByteString>> ask_user_for_download_path(ByteString const& file) const = 0;
    virtual void display_download_confirmation_dialog(StringView download_name, LexicalPath const& path) const = 0;
    virtual void display_error_dialog(StringView error_message) const = 0;
    virtual void open_download(WebView::FileDownloader::Download const& download) const = 0;
    virtual void show_download_in_folder(WebView::FileDownloader::Download const& download) const = 0;
    virtual bool supports_clipboard_type(WebView::Application::ClipboardType type) const = 0;
    virtual Utf16String clipboard_text(WebView::Application::ClipboardType type) const = 0;
    virtual void set_clipboard_text(String text, WebView::Application::ClipboardType type) = 0;
    virtual Web::Clipboard::SystemClipboardItem clipboard_item() const = 0;
    virtual void insert_clipboard_item(Web::Clipboard::SystemClipboardItem item) = 0;
    virtual bool supports_system_menu_bar() const = 0;
    virtual bool supports_vertical_tabs() const = 0;
    virtual bool supports_private_browsing_windows() const = 0;
    virtual bool supports_client_side_window_decorations() const = 0;
    virtual bool platform_reports_scroll_momentum() const = 0;
    virtual bool platform_scrolls_elastically() const = 0;
    virtual bool platform_scrollbar_track_press_jumps() const = 0;
    virtual Vector<String> platform_preferred_languages() const = 0;
    virtual Optional<Gfx::GpuDeviceIdentity> platform_presentation_gpu_device() const = 0;
    virtual Vector<u64> platform_presentation_drm_format_modifiers() const = 0;
    virtual void update_tabs_display() const = 0;
    virtual void rebuild_bookmarks_menu() const = 0;
    virtual void show_bookmark_context_menu(Gfx::IntPoint position, Optional<WebView::BookmarkItem const&> item, Optional<String const&> target_folder_id, Optional<String const&> parent_folder_id) = 0;
    virtual Optional<WebView::Application::BookmarkID> bookmark_item_id_for_context_menu() const = 0;
    virtual NonnullRefPtr<WebView::Application::BookmarkPromise> display_add_bookmark_dialog(Optional<String const&> target_folder_id) const = 0;
    virtual NonnullRefPtr<WebView::Application::BookmarkPromise> display_edit_bookmark_dialog(WebView::BookmarkItem const& current_bookmark, Optional<String const&> parent_folder_id) const = 0;
    virtual NonnullRefPtr<WebView::Application::BookmarkPromise> display_add_bookmark_folder_dialog(Optional<String const&> default_title, Optional<String const&> target_folder_id) const = 0;
    virtual NonnullRefPtr<WebView::Application::BookmarkPromise> display_edit_bookmark_folder_dialog(WebView::BookmarkItem const& current_folder, Optional<String const&> parent_folder_id) const = 0;
    virtual void on_recently_closed_entries_changed() const = 0;
};

// The browser itself: its state, and the helper processes that do its work.
class Application final : public WebView::Application {
    WEB_VIEW_APPLICATION(Application)

public:
    // The host is going away, and takes no more calls.
    void detach_from_host() { m_host = nullptr; }

    View& create_view(View::InitialState, Optional<WebView::CanonicalTraversable&> = {});
    void destroy_view(u64 view_id);
    void destroy_all_views() { m_views.clear(); }

    void toggle_bookmark_without_asking(WebView::ViewImplementation& view) { WebView::Application::toggle_bookmark_for_view(view); }

private:
    explicit Application(ApplicationHost&);

    // ^WebView::Application
    virtual Core::EventLoop& create_platform_event_loop() override;
    virtual NonnullOwnPtr<WebView::MicrophoneAuthorizer> create_microphone_authorizer() override;
    virtual NonnullOwnPtr<WebView::NotificationBackend> create_notification_backend() override;
    virtual void toggle_bookmark_for_view(WebView::ViewImplementation&) override;
    virtual bool captures_browser_crashes() const override { return true; }
    virtual void on_devtools_enabled() const override;
    virtual void on_devtools_disabled() const override;
    virtual WebView::SessionLaunchPolicy launch_restore_policy() const override;
    virtual void create_platform_actions() override;
    virtual Optional<String> ui_font_family() const override;
    virtual Web::CSS::PreferredColorScheme interface_color_scheme() const override;
    virtual Optional<String> system_font_family() const override;
    virtual void focus_page_from_notification(WebView::WebContentPage& page) override;
    virtual void open_url_in_new_tab_from_notification(URL::URL const& url, Web::PageId originating_page, WebView::CanonicalTraversable* canonical_traversable, WebView::IsPrivate is_private) override;
    virtual bool supports_picture_in_picture() const override;
    virtual OwnPtr<WebView::PictureInPictureWindow> create_picture_in_picture_window(WebView::WebContentPage& requesting_page, WebView::CanonicalTraversable& canonical_traversable, Gfx::IntSize video_size) override;
    virtual Optional<WebView::ViewImplementation&> active_web_view() const override;
    virtual Vector<WebView::BookmarkItem::Bookmark> bookmarks_for_all_tabs_in_current_window() const override;
    virtual Optional<WebView::ViewImplementation&> activate_tab_with_url(URL::URL const& url, URL::ExcludeFragment exclude_fragment) const override;
    virtual Optional<WebView::ViewImplementation&> open_blank_new_tab(Web::HTML::ActivateTab activate_tab) const override;
    virtual void open_url_in_new_tab(URL::URL const& url, Web::HTML::ActivateTab activate_tab) const override;
    virtual void open_urls_in_new_tabs(ReadonlySpan<URL::URL> urls) const override;
    virtual void open_url_in_new_window(URL::URL const& url, WebView::IsPrivate is_private) override;
    virtual void open_navigation_in_new_tab(Web::HTML::PreparedNavigationDescriptor navigation, Web::HTML::ActivateTab activate_tab) const override;
    virtual void open_navigation_in_new_window(Web::HTML::PreparedNavigationDescriptor navigation, WebView::IsPrivate is_private) override;
    virtual void resolve_external_url_handler(URL::URL const& url, WebView::ExternalURLHandlerCallback callback) const override;
    virtual NonnullRefPtr<Core::Promise<ByteString>> ask_user_for_download_path(ByteString const& file) const override;
    virtual void display_download_confirmation_dialog(StringView download_name, LexicalPath const& path) const override;
    virtual void display_error_dialog(StringView error_message) const override;
    virtual void open_download(WebView::FileDownloader::Download const& download) const override;
    virtual void show_download_in_folder(WebView::FileDownloader::Download const& download) const override;
    virtual bool supports_clipboard_type(WebView::Application::ClipboardType type) const override;
    virtual Utf16String clipboard_text(WebView::Application::ClipboardType type) const override;
    virtual void set_clipboard_text(String text, WebView::Application::ClipboardType type) override;
    virtual Web::Clipboard::SystemClipboardItem clipboard_item() const override;
    virtual void insert_clipboard_item(Web::Clipboard::SystemClipboardItem item) override;
    virtual bool supports_system_menu_bar() const override;
    virtual bool supports_vertical_tabs() const override;
    virtual bool supports_private_browsing_windows() const override;
    virtual bool supports_client_side_window_decorations() const override;
    virtual bool platform_reports_scroll_momentum() const override;
    virtual bool platform_scrolls_elastically() const override;
    virtual bool platform_scrollbar_track_press_jumps() const override;
    virtual Vector<String> platform_preferred_languages() const override;
    virtual Optional<Gfx::GpuDeviceIdentity> platform_presentation_gpu_device() const override;
    virtual Vector<u64> platform_presentation_drm_format_modifiers() const override;
    virtual void update_tabs_display() const override;
    virtual void rebuild_bookmarks_menu() const override;
    virtual void show_bookmark_context_menu(Gfx::IntPoint position, Optional<WebView::BookmarkItem const&> item, Optional<String const&> target_folder_id, Optional<String const&> parent_folder_id) override;
    virtual Optional<WebView::Application::BookmarkID> bookmark_item_id_for_context_menu() const override;
    virtual NonnullRefPtr<WebView::Application::BookmarkPromise> display_add_bookmark_dialog(Optional<String const&> target_folder_id) const override;
    virtual NonnullRefPtr<WebView::Application::BookmarkPromise> display_edit_bookmark_dialog(WebView::BookmarkItem const& current_bookmark, Optional<String const&> parent_folder_id) const override;
    virtual NonnullRefPtr<WebView::Application::BookmarkPromise> display_add_bookmark_folder_dialog(Optional<String const&> default_title, Optional<String const&> target_folder_id) const override;
    virtual NonnullRefPtr<WebView::Application::BookmarkPromise> display_edit_bookmark_folder_dialog(WebView::BookmarkItem const& current_folder, Optional<String const&> parent_folder_id) const override;
    virtual void on_recently_closed_entries_changed() const override;

    ApplicationHost* m_host { nullptr };
    HashMap<u64, NonnullOwnPtr<View>> m_views;
};

}
