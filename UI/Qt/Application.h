/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/Platform.h>
#include <LibURL/URL.h>
#include <LibWebCommon/Page/PageId.h>
#include <LibWebView/Application.h>
#include <LibWebView/BrowsingSession.h>
#include <UI/Qt/BrowserWindow.h>
#include <UI/Qt/CrashReportNotifier.h>

#include <QApplication>

class QAction;
class QMenu;
class QMenuBar;
class QWidget;

namespace Ladybird {

class ProcessManagerWindow;

struct WindowConfiguration {
    Optional<Web::DevicePixels> x {};
    Optional<Web::DevicePixels> y {};
    Optional<Web::DevicePixels> width {};
    Optional<Web::DevicePixels> height {};
    Optional<bool> maximized {};
};

enum class ShowWindow {
    No,
    Yes,
};

class Application final : public WebView::Application {
    WEB_VIEW_APPLICATION(Application)

public:
    virtual ~Application() override;

    Function<void(URL::URL)> on_open_file;

    // A page's initial navigation, when there is one, starts in place of the first initial URL.
    BrowserWindow& new_window(Vector<URL::URL> const& initial_urls, WindowConfiguration const& = {}, BrowserWindow::IsPopupWindow is_popup_window = BrowserWindow::IsPopupWindow::No, WebView::IsPrivate = WebView::IsPrivate::No, Tab* parent_tab = nullptr, Optional<WebView::CanonicalTraversable&> traversable = {}, ShowWindow = ShowWindow::Yes, Optional<Web::HTML::PreparedNavigationDescriptor> initial_navigation = {});
    WindowConfiguration configuration_for_new_window() const;

    void open_new_tab();
    void open_new_window(WebView::IsPrivate);
    void reopen_recently_closed_tab();
    void open_file();

    void restart_private_browsing_session();
    void focus_location_editor();
    void show_process_manager();
    void quit();
    bool confirm_stop_active_downloads(QWidget* parent = nullptr);

    CrashReportNotifier& crash_report_notifier() { return m_crash_report_notifier; }
    void review_crash_report(ByteString const& report_name);

    BrowserWindow& active_window() const { return *m_active_window; }
    void set_active_window(BrowserWindow& window) { m_active_window = &window; }
    BrowserWindow* active_window_if_any() const { return m_active_window; }
    BrowserWindow* non_private_window_if_any() const;

    Tab* active_tab() const { return m_active_window ? m_active_window->current_tab() : nullptr; }
    void update_reopen_recently_closed_actions() const;

    enum class ForBrowserWindow : u8 {
        No,
        Yes,
    };
    QMenuBar* create_application_menu_bar(ForBrowserWindow);

    QMenu* bookmarks_menu();
    QMenu* history_menu();
    QMenu* inspect_menu();
    QMenu* debug_menu();
    QMenu* help_menu();

    QAction* new_tab_action();
    QAction* new_window_action();
    QAction* new_private_window_action();
    QAction* reopen_recently_closed_tab_action();
    QAction* close_current_tab_action();
    QAction* open_next_tab_action();
    QAction* open_previous_tab_action();
    QAction* open_file_action();
    QAction* open_settings_action();
    QAction* open_downloads_action();
    QAction* find_in_page_action();
    QAction* zoom_in_action();
    QAction* zoom_out_action();
    QAction* reset_zoom_action();
    QAction* quit_action();

private:
    explicit Application();

    virtual void create_platform_options(WebView::BrowserOptions&, WebView::RequestServerOptions&, WebView::WebContentOptions&) override;
    virtual Core::EventLoop& create_platform_event_loop() override;
    virtual void create_platform_actions() override;

    virtual Optional<String> ui_font_family() const override;
#if !defined(AK_OS_MACOS)
    virtual Optional<String> system_font_family() const override;
#endif

    virtual Optional<WebView::ViewImplementation&> active_web_view() const override;
    virtual Vector<WebView::ViewImplementation&> active_window_web_views() const override;
    virtual bool activate_tab_with_url(URL::URL const&) const override;

    virtual Optional<WebView::ViewImplementation&> open_blank_new_tab(Web::HTML::ActivateTab) const override;
    virtual void open_url_in_new_tab(URL::URL const&, Web::HTML::ActivateTab) const override;
    virtual void open_urls_in_new_tabs(ReadonlySpan<URL::URL>) const override;
    virtual void open_url_in_new_window(URL::URL const&, WebView::IsPrivate) override;
    virtual void open_navigation_in_new_tab(Web::HTML::PreparedNavigationDescriptor, Web::HTML::ActivateTab) const override;
    virtual void open_navigation_in_new_window(Web::HTML::PreparedNavigationDescriptor, WebView::IsPrivate) override;

    virtual void resolve_external_url_handler(URL::URL const&, WebView::ExternalURLHandlerCallback) const override;

    virtual Optional<ByteString> ask_user_for_download_path(ByteString const& file) const override;
    virtual void display_download_confirmation_dialog(StringView download_name, LexicalPath const& path) const override;
    virtual void display_error_dialog(StringView error_message) const override;
    virtual void open_download(WebView::FileDownloader::Download const&) const override;
    virtual void show_download_in_folder(WebView::FileDownloader::Download const&) const override;

    virtual void display_crash_report_notification(ByteString const& report_name) override;

    virtual bool supports_clipboard_type(ClipboardType) const override;
    virtual Utf16String clipboard_text(ClipboardType) const override;
    virtual void set_clipboard_text(String, ClipboardType = ClipboardType::Text) override;

    virtual Web::Clipboard::SystemClipboardItem clipboard_item() const override;
    virtual void insert_clipboard_item(Web::Clipboard::SystemClipboardItem) override;

    virtual bool supports_system_menu_bar() const override;
    virtual bool supports_vertical_tabs() const override { return true; }
    virtual bool supports_private_browsing_windows() const override { return true; }
    virtual bool supports_client_side_window_decorations() const override
    {
#if defined(AK_OS_MACOS)
        return false;
#else
        return true;
#endif
    }

    virtual bool platform_reports_scroll_momentum() const override;

    virtual void update_tabs_display() const override;

    virtual void rebuild_bookmarks_menu() const override;
    virtual void show_bookmark_context_menu(Gfx::IntPoint, Optional<WebView::BookmarkItem const&>, Optional<String const&> target_folder_id, Optional<String const&> parent_folder_id) override;
    virtual Optional<BookmarkID> bookmark_item_id_for_context_menu() const override;
    virtual NonnullRefPtr<BookmarkPromise> display_add_bookmark_dialog(Optional<String const&> target_folder_id = {}) const override;
    virtual NonnullRefPtr<BookmarkPromise> display_edit_bookmark_dialog(WebView::BookmarkItem const& current_bookmark, Optional<String const&> parent_folder_id) const override;
    virtual NonnullRefPtr<BookmarkPromise> display_add_bookmark_folder_dialog(Optional<String const&> default_title = {}, Optional<String const&> target_folder_id = {}) const override;
    virtual NonnullRefPtr<BookmarkPromise> display_edit_bookmark_folder_dialog(WebView::BookmarkItem const& current_folder, Optional<String const&> parent_folder_id) const override;

    virtual void on_devtools_enabled() const override;
    virtual void on_devtools_disabled() const override;
    virtual void on_recently_closed_entries_changed() const override;

    OwnPtr<QApplication> m_application;
    BrowserWindow* m_active_window { nullptr };
    OwnPtr<ProcessManagerWindow> m_process_manager_window;
    CrashReportNotifier m_crash_report_notifier;
};

}
