/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/Platform.h>
#include <Hub/Application.h>
#include <Hub/ConnectionFromUI.h>
#include <LibCore/Platform/ProcessStatistics.h>
#include <LibURL/URL.h>
#include <LibWebCommon/Page/PageId.h>
#include <LibWebView/Application.h>
#include <LibWebView/Autocomplete.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/CrashReportOutbox.h>
#include <LibWebView/ProcessSnapshot.h>
#include <UI/Qt/ApplicationActions.h>
#include <UI/Qt/BrowserWindow.h>
#include <UI/Qt/HubConnection.h>

#include <QApplication>
#include <QPointer>

class QAction;
class QMenu;
class QMenuBar;
class QWidget;

namespace Ladybird {

class WebUIPages;

class Bookmarks;
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

class Application final
    : public Hub::ApplicationHost
    , public WebView::AutocompleteHost
    , public WebView::CrashReportOutboxObserver {
    friend class ApplicationActions;
    friend class Bookmarks;
    friend class BookmarksPage;
    friend class RemoteDownloads;
    friend class RemoteView;
    friend class HubConnection;

public:
    AK_ALLOC_WITH_KMALLOC;

    struct BookmarkDialogResult {
        Variant<BookmarkItem::Bookmark, BookmarkItem::Folder> data;
        Optional<String> target_folder_id;
    };
    using BookmarkPromise = Core::Promise<BookmarkDialogResult>;
    using ClipboardType = WebView::Application::ClipboardType;

    static ErrorOr<NonnullOwnPtr<Application>> create(Main::Arguments const&);
    static Application& the() { return *s_the; }
#ifdef LADYBIRD_HUB_IN_OWN_PROCESS
    static ErrorOr<ByteString> hub_executable_path();
    static WebView::BrowserOptions const& browser_options() { return s_the->m_browser_options; }
#else
    static Hub::Application& hub() { return Hub::Application::the(); }
    static WebView::BrowserOptions const& browser_options() { return WebView::Application::browser_options(); }
#endif
    bool has_translator() const;
    WebView::MicrophoneAuthorizer* microphone_authorizer() { return m_microphone_authorizer.ptr(); }
    // The status to exit with right away, if the browser has nothing for this process to do.
    Optional<int> exit_status_if_done() const;
    ErrorOr<int> execute();

    Function<void(Vector<URL::URL> const&)> on_new_tab;
    Function<void(Vector<URL::URL> const&)> on_new_window;
    static WebView::Settings& settings() { return *s_the->m_settings; }
    static Bookmarks& bookmarks() { return *s_the->m_bookmarks; }
    static BookmarkStore& bookmark_store() { return *s_the->m_bookmark_store; }

    // The windows and tabs of each kind of window, which this run keeps for the next one unless they are private.
    static SessionStore& session_store(WebView::IsPrivate);

    // Browsing history and the favicons it, the bookmarks and the open tabs show. Private windows keep no history.
    static WebView::HistoryStore& history_store() { return *s_the->m_history_store; }
    static WebView::FaviconStore& favicon_store(WebView::IsPrivate);
    void remove_unreferenced_favicons(HashTable<String> referenced_hashes);

    // The location bars make their suggestions from the history, and from the search engine through the Hub.
    virtual WebView::AutocompleteService& autocomplete_service() override { return *m_autocomplete_service; }
    virtual WebView::Settings& autocomplete_settings() override { return *m_settings; }
    virtual Function<void()> fetch_remote_suggestions(WebView::SearchEngine const&, String const& query, WebView::IsPrivate, Function<void(RemoteResponse)>) override;

    static WebUIPages& web_ui_pages() { return *s_the->m_web_ui_pages; }
    String const& command_line() const { return m_command_line; }

    virtual ~Application() override;

    Function<void(URL::URL)> on_open_file;

    // A page's initial navigation, when there is one, starts in place of the first initial URL.
    BrowserWindow& new_window(Vector<URL::URL> const& initial_urls, WindowConfiguration const& = {}, BrowserWindow::IsPopupWindow is_popup_window = BrowserWindow::IsPopupWindow::No, WebView::IsPrivate = WebView::IsPrivate::No, Optional<u64> hub_view_id = {}, ShowWindow = ShowWindow::Yes, Optional<Web::HTML::PreparedNavigationDescriptor> initial_navigation = {});
    WindowConfiguration configuration_for_new_window() const;

    // Opens the windows startup recovery kept open, and returns the frontmost one, if any.
    BrowserWindow* restore_previous_session(WindowConfiguration const& frontmost_window_configuration);
    // After the last run ended unexpectedly, offers to restore the tabs it left open that didn't come back by themselves.
    void offer_to_restore_unexpectedly_closed_tabs(BrowserWindow&);
    void open_startup_tabs(BrowserWindow&, Vector<URL::URL> const&);
    SessionLaunchPolicy launch_restore_policy() const;

    void open_new_tab();
    void open_new_window(WebView::IsPrivate);
    enum class ReopenClosedWindow {
        InNewWindow,
        InTargetWindow,
    };
    // Reopens the given closed tab or window, or else the one closed most recently.
    void reopen_recently_closed_tab(BrowserWindow* target_window, Optional<SessionWindowId> = {}, ReopenClosedWindow = ReopenClosedWindow::InNewWindow);
    void open_file();

    void restart_private_browsing_session();
    void focus_location_editor();
    void show_process_manager();
    // Asks every window's pages if they may unload, one window at a time. Once all of them agree, it confirms stopping
    // the downloads and quits; if a page refuses or the downloads are kept, the quit is canceled and no window closes.
    void quit();
    bool is_quit_in_progress() const { return m_quit_in_progress; }
    bool is_finishing_quit() const { return m_quit_is_finishing; }
    bool confirm_stop_active_downloads(QWidget* parent = nullptr);

    void update_feedback_buttons();
    void report_active_view() const;

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
    QAction* search_tabs_action();
    QAction* move_focus_action();
    QAction* open_file_action();
    QAction* open_settings_action();
    QAction* open_downloads_action();
    QAction* toggle_bookmark_bar_action();
    QAction* toggle_sidebar_action();
    QAction* open_location_action();
    QAction* reload_action();
    QAction* reload_bypassing_cache_action();
    QAction* stop_action();
    QAction* find_next_action();
    QAction* find_previous_action();
    QAction* find_in_page_action();
    QAction* zoom_in_action();
    QAction* zoom_out_action();
    QAction* reset_zoom_action();
    QAction* quit_action();

    void set_clipboard_text(String, ClipboardType = ClipboardType::Text);
    void insert_clipboard_item(Web::Clipboard::SystemClipboardItem);
    virtual bool supports_private_browsing_windows() const override { return true; }

private:
    void present_window(BrowserWindow&, WindowConfiguration const&, ShowWindow);
    explicit Application();

    void ask_next_window_to_close_for_private_session_restart();
    void finish_private_session_restart();
    void cancel_private_session_restart();

    void ask_next_window_to_close_for_quit();
    bool ask_windows_yet_to_agree_to_quit();
    void finish_quit();
    void cancel_quit();

    virtual void will_create_event_loop(Main::Arguments&) override;
    virtual void did_create_event_loop(Core::EventLoop&) override;
    virtual void create_platform_actions() override;

    virtual Optional<String> ui_font_family() const override;
    virtual Web::CSS::PreferredColorScheme interface_color_scheme() const override;
    void show_bookmark_popover();
    virtual Optional<String> system_font_family() const override;

    OwnPtr<WebView::NotificationBackend> create_notification_backend();
    WebView::NotificationBackend& notification_backend();
    void focus_view_from_notification(u64 view_id);
    void open_url_in_new_tab_from_notification(URL::URL const&, Optional<u64> view_id, WebView::IsPrivate);

    virtual bool supports_picture_in_picture() const override { return true; }
    void open_picture_in_picture_window(u64 view_id, u64 owner_view_id, Gfx::IntSize video_size);
    void hide_picture_in_picture_window(u64 view_id);
    void set_picture_in_picture_video_size(u64 view_id, Gfx::IntSize);
    void close_picture_in_picture_window(u64 view_id);

    Vector<BookmarkItem::Bookmark> bookmarks_for_all_tabs_in_active_window() const;
    Tab* activate_tab_with_url(URL::URL const&, URL::ExcludeFragment) const;
    void activate_or_open_tab_with_url(URL::URL const&);

    BrowserWindow* window_of_view(u64 view_id) const;
    BrowserWindow* window_for_new_tab(Optional<u64> source_view_id, WebView::IsPrivate) const;
    void open_view_in_new_tab(u64 view_id, Optional<u64> source_view_id, WebView::IsPrivate, Web::HTML::ActivateTab);
    void open_url_in_new_tab(URL::URL const&, Web::HTML::ActivateTab, Optional<u64> source_view_id, WebView::IsPrivate) const;
    void open_urls_in_new_tabs(ReadonlySpan<URL::URL>) const;
    void open_url_in_new_window(URL::URL const&, WebView::IsPrivate);
    void open_navigation_in_new_tab(Web::HTML::PreparedNavigationDescriptor, Web::HTML::ActivateTab, Optional<u64> source_view_id, WebView::IsPrivate) const;
    void open_navigation_in_new_window(Web::HTML::PreparedNavigationDescriptor, WebView::IsPrivate);

    virtual void resolve_external_url_handler(URL::URL const&, WebView::ExternalURLHandlerCallback) const override;
    void resolve_external_url_handler_for_hub(u64 request_id, URL::URL const&);
    void launch_external_url(u64 handler_id, u64 request_id, URL::URL const&);
    void release_external_url_handler(u64 handler_id);

    void ask_user_for_download_path(u64 request_id, ByteString const& file) const;
    void display_download_confirmation_dialog(StringView download_name, ByteString const& path) const;
    void display_error_dialog(StringView error_message) const;
    void open_file(String const& path) const;

    virtual void crash_reports_changed() override;

    virtual bool supports_clipboard_type(ClipboardType) const override;
    Utf16String clipboard_text(ClipboardType) const;

    Web::Clipboard::SystemClipboardItem clipboard_item() const;

    virtual bool supports_system_menu_bar() const override;
    virtual bool supports_vertical_tabs() const override { return true; }
    virtual bool supports_client_side_window_decorations() const override
    {
#if defined(AK_OS_MACOS)
        return false;
#else
        return true;
#endif
    }

    virtual bool platform_reports_scroll_momentum() const override;
    virtual bool platform_scrolls_elastically() const override;
    virtual bool platform_scrollbar_track_press_jumps() const override;
    virtual Vector<String> platform_preferred_languages() const override;
    virtual Optional<Gfx::GpuDeviceIdentity> platform_presentation_gpu_device() const override;
    virtual Vector<u64> platform_presentation_drm_format_modifiers() const override;

    void update_tabs_display() const;

    void rebuild_bookmarks_menu() const;
    void show_bookmark_context_menu(Gfx::IntPoint, Optional<BookmarkItem const&>, Optional<String const&> target_folder_id, Optional<String const&> parent_folder_id);
    NonnullRefPtr<BookmarkPromise> display_add_bookmark_dialog(Optional<String const&> target_folder_id = {}) const;
    NonnullRefPtr<BookmarkPromise> display_edit_bookmark_dialog(BookmarkItem const& current_bookmark, Optional<String const&> parent_folder_id) const;
    NonnullRefPtr<BookmarkPromise> display_add_bookmark_folder_dialog(Optional<String const&> default_title = {}, Optional<String const&> target_folder_id = {}) const;
    NonnullRefPtr<BookmarkPromise> display_edit_bookmark_folder_dialog(BookmarkItem const& current_folder, Optional<String const&> parent_folder_id) const;

    void did_enable_devtools() const;
    void did_disable_devtools() const;
    void on_recently_closed_entries_changed() const;
    void did_clear_history_since(UnixDateTime);
    void create_session_store(ByteString const& data_directory, bool persists_sessions);
    void create_history_store(ByteString const& data_directory, bool uses_sql_database);
    void quit_for_signal();

    static Application* s_the;

    String m_command_line;

#ifdef LADYBIRD_HUB_IN_OWN_PROCESS
    ErrorOr<void> connect_to_hub(Main::Arguments const&);
    Hub::UIInfo current_ui_info() const;
    Vector<WebView::ProcessSnapshot> process_snapshots();

    // NB: QApplication holds on to these by reference, so they must live as long as it does.
    Main::Arguments m_arguments_for_qt;

    Core::EventLoop* m_event_loop { nullptr };
    WebView::BrowserOptions m_browser_options;
    Optional<int> m_exit_status_if_done;
    Core::Platform::ProcessStatistics m_ui_process_statistics;
#    if defined(AK_OS_MACOS)
    OwnPtr<Core::TimeZoneWatcher> m_time_zone_watcher;
#    endif
#else
    OwnPtr<Hub::Application> m_hub;
    RefPtr<Hub::ConnectionFromUI> m_connection_from_ui;
#endif
    WebView::Translator* translator() { return m_translator.ptr(); }
    WebView::SpellChecker* spell_checker() { return m_spell_checker.ptr(); }

    OwnPtr<WebView::Translator> m_translator;
    OwnPtr<WebView::SpellChecker> m_spell_checker;
    OwnPtr<WebView::MicrophoneAuthorizer> m_microphone_authorizer;
    OwnPtr<WebView::Settings> m_settings;
    OwnPtr<BookmarkStore> m_bookmark_store;
    OwnPtr<SessionStore> m_session_store;
    OwnPtr<SessionStore> m_private_session_store;
    OwnPtr<WebView::FaviconStore> m_favicon_store;
    OwnPtr<WebView::FaviconStore> m_private_favicon_store;
    OwnPtr<WebView::HistoryStore> m_history_store;
    OwnPtr<WebView::AutocompleteService> m_autocomplete_service;
    bool m_should_remove_unreferenced_favicons { false };
    Vector<int> m_session_signal_handlers;
    OwnPtr<Bookmarks> m_bookmarks;
    // NB: Declared after the stores and the bookmarks, so the pages that show them are gone first.
    OwnPtr<WebUIPages> m_web_ui_pages;
    OwnPtr<ApplicationActions> m_application_actions;
    RefPtr<HubConnection> m_hub_connection;
    OwnPtr<QApplication> m_application;
    HashMap<u64, NonnullOwnPtr<WebView::PictureInPictureWindow>> m_picture_in_picture_windows;
    OwnPtr<WebView::NotificationBackend> m_notification_backend;
    HashMap<u64, NonnullRefPtr<WebView::ExternalURLHandler>> m_external_url_handlers;
    u64 m_next_external_url_handler_id { 1 };
    BrowserWindow* m_active_window { nullptr };
    OwnPtr<ProcessManagerWindow> m_process_manager_window;
    RefPtr<Action> m_open_task_manager_action;

    bool m_private_session_restart_in_progress { false };
    bool m_quit_in_progress { false };
    bool m_quit_is_finishing { false };
    Vector<QPointer<BrowserWindow>> m_windows_awaiting_quit_preflight;
};

}
