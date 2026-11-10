/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Hex.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/Environment.h>
#include <LibCore/EventLoop.h>
#include <LibCore/GeolocationProvider.h>
#include <LibCore/Process.h>
#include <LibCore/Socket.h>
#include <LibCore/System.h>
#include <LibDatabase/Database.h>
#include <LibHTTP/HeaderList.h>
#include <LibIPC/Encoder.h>
#include <LibIPC/Transport.h>
#include <LibURL/InternalURLs.h>
#include <LibURL/URL.h>
#include <LibWebCommon/WebView/Utilities.h>
#include <LibWebView/AutocompleteService.h>
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/CrashReport.h>
#include <LibWebView/CrashReportStore.h>
#include <LibWebView/FaviconStore.h>
#include <LibWebView/HistoryStore.h>
#include <LibWebView/NotificationBackend.h>
#include <LibWebView/PageTranslation.h>
#include <LibWebView/PictureInPictureManager.h>
#include <LibWebView/ReportSubmission.h>
#include <UI/Qt/SystemAppearance.h>
#if defined(AK_OS_MACOS)
#    include <AK/AtomicRefCounted.h>
#    include <LibCore/MachPort.h>
#    include <LibIPC/MachBootstrapListener.h>
#    include <LibIPC/TransportBootstrapMach.h>
#    include <LibIPC/TransportMachPort.h>
#    include <LibThreading/Thread.h>
#    include <LibWebView/NotificationBackendMacOS.h>
#    include <mach/mach.h>
#    include <sys/wait.h>
#elif defined(AK_OS_LINUX) || defined(AK_OS_FREEBSD)
#    include <LibWebView/NotificationBackendFreedesktop.h>
#endif
#include <LibWebView/NoOpNotificationBackend.h>
#include <LibWebView/Translator.h>
#include <LibWebView/URL.h>
#include <LibWebView/Utilities.h>
#include <LibWebView/WebContentPage.h>
#include <UI/Qt/Application.h>
#include <UI/Qt/BookmarkDialog.h>
#include <UI/Qt/Bookmarks.h>
#include <UI/Qt/ChromeLayout.h>
#include <UI/Qt/DownloadsPage.h>
#include <UI/Qt/EventLoopImplementationQt.h>
#include <UI/Qt/ExternalURLHandler.h>
#include <UI/Qt/HistoryPage.h>
#include <UI/Qt/Menu.h>
#include <UI/Qt/PictureInPictureWindow.h>
#include <UI/Qt/PlainText.h>
#include <UI/Qt/ProcessManagerWindow.h>
#include <UI/Qt/RemoteDownloads.h>
#include <UI/Qt/SessionStore.h>
#include <UI/Qt/StringUtils.h>
#include <UI/Qt/Tab.h>
#include <UI/Qt/Theme.h>
#include <UI/Qt/VersionPage.h>
#include <UI/Qt/WebContentView.h>
#include <UI/Qt/WebUIPage.h>

#if defined(AK_OS_MACOS)
#    include <UI/Qt/ApplicationIcon.h>
#    include <UI/Qt/LaunchURLsMacOS.h>
#    include <UI/Qt/MacWindow.h>
#    include <UI/Qt/MicrophoneAuthorizerMacOS.h>
#    include <UI/Qt/ScrollbarPreferencesMac.h>
#    include <UI/Qt/SpellCheckerMac.h>
#endif

#if defined(LADYBIRD_HAS_TRANSLATOR)
#    include <UI/Qt/TranslatorMac.h>
#endif

#if defined(AK_OS_LINUX)
#    include <UI/Qt/X11Window.h>
#endif

#include <QAction>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileOpenEvent>
#include <QFont>
#include <QKeySequence>
#include <QLocale>
#include <QLoggingCategory>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QMimeDatabase>
#include <QPointer>
#include <QSaveFile>
#include <QScreen>
#include <QShortcut>
#include <QStandardPaths>
#include <QTimer>

#if defined(AK_OS_MACOS)
#    include <QUtiMimeConverter>
#endif

#if defined(AK_OS_MACOS) || defined(AK_OS_WINDOWS)
#    include <LibCore/TimeZoneWatcher.h>
#endif

#if defined(AK_OS_WINDOWS)
#    include <AK/Windows.h>
#    include <QAbstractNativeEventFilter>
#endif

template<>
constexpr bool AllocatedWithSystemAllocator<QWidget> = true;

namespace Ladybird {

#if !defined(Q_OS_MACOS) && !defined(Q_OS_WIN)
// Orca 49.7 removed support for the ~/.local/share/orca/orca-scripts/ directory as a means to directly load custom
// user/app-supplied scripts. We still deliver our custom script there — but to make it work, we plumb into the means
// Orca now uses for loading customer user-supplied scripts: Orca's orca-customizations.py file. So, in that file, we
// mark/delimit the block we own — to ensure we don't change any content outside that (the user's own manual additions).
static constexpr QByteArrayView ORCA_CUSTOMIZATIONS_BEGIN_MARKER = "# --- Ladybird-bootstrap-begin ---";
static constexpr QByteArrayView ORCA_CUSTOMIZATIONS_END_MARKER = "# --- Ladybird-bootstrap-end ---";

static void install_orca_customizations_bootstrap(QString const& orca_dir)
{
    QFile bootstrap_resource(QStringLiteral(":/OrcaScripts/orca_customizations_bootstrap.py"));
    if (!bootstrap_resource.open(QIODevice::ReadOnly))
        return;

    QByteArray managed_region;
    managed_region.append(ORCA_CUSTOMIZATIONS_BEGIN_MARKER);
    managed_region.append('\n');
    managed_region.append(bootstrap_resource.readAll());
    if (!managed_region.endsWith('\n'))
        managed_region.append('\n');
    managed_region.append(ORCA_CUSTOMIZATIONS_END_MARKER);
    managed_region.append('\n');

    auto customizations_path = orca_dir + QStringLiteral("/orca-customizations.py");

    QByteArray existing_content;
    if (QFile::exists(customizations_path)) {
        QFile existing(customizations_path);
        if (existing.open(QIODevice::ReadOnly))
            existing_content = existing.readAll();
    }

    auto begin_idx = existing_content.indexOf(ORCA_CUSTOMIZATIONS_BEGIN_MARKER);
    auto end_idx = existing_content.indexOf(ORCA_CUSTOMIZATIONS_END_MARKER);

    QByteArray new_content;
    if (begin_idx < 0 || end_idx < 0 || end_idx < begin_idx) {
        new_content = existing_content;
        if (!new_content.isEmpty() && !new_content.endsWith('\n'))
            new_content.append('\n');
        new_content.append(managed_region);
    } else {
        auto end_of_end_line = existing_content.indexOf('\n', end_idx);
        if (end_of_end_line < 0)
            end_of_end_line = existing_content.size();
        else
            ++end_of_end_line;
        new_content = existing_content.left(begin_idx)
            + managed_region
            + existing_content.mid(end_of_end_line);
    }

    if (new_content == existing_content)
        return;

    // Write atomically: the file holds the user's own customizations outside the managed region (preserved in
    // new_content), and QFile::open(WriteOnly) truncates it at open() — so a crash or short write before write()
    // completes would destroy them. QSaveFile writes to a temporary and renames only on commit().
    QSaveFile out(customizations_path);
    if (out.open(QIODevice::WriteOnly)) {
        out.write(new_content);
        out.commit();
    }
}

static void install_orca_scripts()
{
    static constexpr auto scripts = Array {
        "__init__.py"sv,
        "script.py"sv,
        "script_utilities.py"sv,
    };

    auto orca_dir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
        + QStringLiteral("/orca");
    auto dest_dir = QDir(orca_dir + QStringLiteral("/orca-scripts/Ladybird"));

    if (!dest_dir.exists())
        dest_dir.mkpath(QStringLiteral("."));

    for (auto script_name : scripts) {
        auto resource_path = QStringLiteral(":/OrcaScripts/Ladybird/%1")
                                 .arg(QString::fromUtf8(script_name.characters_without_null_termination(), script_name.length()));
        auto dest_path = dest_dir.filePath(
            QString::fromUtf8(script_name.characters_without_null_termination(), script_name.length()));

        QFile resource(resource_path);
        if (!resource.open(QIODevice::ReadOnly))
            continue;

        auto resource_data = resource.readAll();

        QFile existing(dest_path);
        if (existing.exists() && existing.open(QIODevice::ReadOnly)) {
            if (existing.readAll() == resource_data)
                continue;
            existing.close();
        }

        QFile out(dest_path);
        if (out.open(QIODevice::WriteOnly))
            out.write(resource_data);
    }

    install_orca_customizations_bootstrap(orca_dir);
}
#endif

#if defined(AK_OS_WINDOWS)
class NativeWindowsTimeChangeEventFilter : public QAbstractNativeEventFilter {
public:
    NativeWindowsTimeChangeEventFilter(Core::TimeZoneWatcher& time_zone_watcher)
        : m_time_zone_watcher(time_zone_watcher)
    {
    }

    bool nativeEventFilter(QByteArray const& event_type, void* message, qintptr*) override
    {
        if (event_type == QByteArrayLiteral("windows_generic_MSG")) {
            auto msg = static_cast<MSG*>(message);
            if (msg->message == WM_TIMECHANGE) {
                m_time_zone_watcher.on_time_zone_changed();
            }
        }
        return false;
    }

private:
    Core::TimeZoneWatcher& m_time_zone_watcher;
};

#endif

static constexpr int new_window_cascade_offset = 32;

#if defined(AK_OS_MACOS)
static bool has_visible_browser_window()
{
    for (auto* widget : QApplication::topLevelWidgets()) {
        if (as_if<BrowserWindow>(widget) && widget->isVisible())
            return true;
    }
    return false;
}
#endif

class LadybirdQApplication : public QApplication {
public:
    AK_ALLOC_WITH_KMALLOC;

    explicit LadybirdQApplication(Main::Arguments& arguments)
        : QApplication(arguments.argc, arguments.argv)
        , m_application_widget(make<QWidget>())
    {
#if defined(AK_OS_MACOS)
        setQuitOnLastWindowClosed(false);
        install_appkit_event_capture();
#endif
        installEventFilter(&m_plain_text_event_filter);

        // Suppress some warnings that end up flooding the console when Orca is in use. QSpiAccessibleBridge logs both
        // (1) one-time at startup, a couple warnings for some AT-SPI2 methods it doesn't implement, and (2) for *every*
        // single Orca navigation action a user makes *every* time, warns about upstream Qt gaps in scroll-related stuff
        // we can't fix from here. So, this silences all those warnings. But if/when you *do* actually want to see the
        // messages — to debug Qt's bridge behavior, or whatever — you can override this with QT_LOGGING_RULES.
        QLoggingCategory::setFilterRules(QStringLiteral("qt.accessibility.atspi=false"));
    }

    virtual bool event(QEvent* event) override
    {
        auto& application = Application::the();
        auto const event_type = event->type();

#if defined(AK_OS_WINDOWS)
        static Optional<NativeWindowsTimeChangeEventFilter> time_change_event_filter {};
        if (auto time_zone_watcher = Application::hub().time_zone_watcher(); !time_change_event_filter.has_value() && time_zone_watcher.has_value()) {
            time_change_event_filter.emplace(time_zone_watcher.value());
            installNativeEventFilter(&time_change_event_filter.value());
        }
#endif

        switch (event_type) {
        case QEvent::FileOpen: {
            if (!application.on_open_file)
                break;

            auto const& open_event = *static_cast<QFileOpenEvent const*>(event);
            auto const qurl = open_event.url();
            if (!qurl.isEmpty()) {
                application.on_open_file(ak_url_from_qurl(qurl));
                break;
            }

            auto file = ak_string_from_qstring(open_event.file());

            if (auto file_url = WebView::sanitize_url(file); file_url.has_value())
                application.on_open_file(file_url.release_value());
            break;
        }

#if defined(AK_OS_MACOS)
        case QEvent::Quit:
            // The system asks the application to quit (from the Dock, or at logout) with a spontaneous Quit event, and
            // so does QApplication::quit(), which on macOS terminates through the system. So until every window's pages
            // have agreed, the termination is canceled and quit() asks the pages instead — and then finishes by itself.
            if (event->spontaneous() && !application.is_finishing_quit()) {
                application.quit();
                event->ignore();
                return true;
            }
            break;
#endif

        default:
            break;
        }

        auto handled = QApplication::event(event);

#if defined(AK_OS_MACOS)
        if (event_type == QEvent::ApplicationActivate && !has_visible_browser_window())
            application.open_new_window(WebView::IsPrivate::No);
#endif

        return handled;
    }

    void create_application_actions()
    {
        auto& application = ApplicationActions::the();

        auto make_action = [&](QString const& text) {
            auto* action = new QAction(text, m_application_widget);
            action->setShortcutVisibleInContextMenu(true);
            return action;
        };

        m_new_tab_action = make_action("New &Tab");
        m_new_tab_action->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_T));
        QObject::connect(m_new_tab_action, &QAction::triggered, this, []() { Application::the().open_new_tab(); });

        m_new_window_action = make_action("New &Window");
        m_new_window_action->setShortcuts(QKeySequence::keyBindings(QKeySequence::StandardKey::New));
        QObject::connect(m_new_window_action, &QAction::triggered, this, []() { Application::the().open_new_window(WebView::IsPrivate::No); });

        m_new_private_window_action = make_action("New Pri&vate Window");
        m_new_private_window_action->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N));
        QObject::connect(m_new_private_window_action, &QAction::triggered, this, []() { Application::the().open_new_window(WebView::IsPrivate::Yes); });

        m_reopen_recently_closed_tab_action = make_action("&Reopen Closed Tab");
        m_reopen_recently_closed_tab_action->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_T));
        QObject::connect(m_reopen_recently_closed_tab_action, &QAction::triggered, this, []() { Application::the().reopen_recently_closed_tab(Application::the().active_window_if_any()); });
        update_reopen_recently_closed_action();

        m_close_current_tab_action = make_action("&Close Current Tab");
        m_close_current_tab_action->setShortcuts(QKeySequence::keyBindings(QKeySequence::StandardKey::Close));
        QObject::connect(m_close_current_tab_action, &QAction::triggered, this, []() {
            auto* tab = Application::the().active_tab();
            if (!tab)
                return;

            // The keyboard shortcut never closes a pinned tab: It selects the first unpinned tab instead. Choosing the
            // menu item still closes the tab.
            auto from_keyboard = QGuiApplication::keyboardModifiers() & (Qt::ControlModifier | Qt::MetaModifier);
            if (tab->is_pinned() && from_keyboard) {
                Application::the().active_window_if_any()->activate_first_unpinned_tab();
                return;
            }
            tab->request_close();
        });

        m_open_next_tab_action = make_action("Open &Next Tab");
        m_open_next_tab_action->setShortcuts({
            QKeySequence(Qt::CTRL | Qt::Key_PageDown),
            QKeySequence(Qt::CTRL | Qt::Key_Tab),
#if defined(AK_OS_MACOS)
            QKeySequence(Qt::META | Qt::Key_Tab),
            QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_BracketRight),
            QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_Right),
#endif
        });
        QObject::connect(m_open_next_tab_action, &QAction::triggered, this, []() {
            if (auto* window = Application::the().active_window_if_any())
                window->open_next_tab();
        });

        m_open_previous_tab_action = make_action("Open &Previous Tab");
        m_open_previous_tab_action->setShortcuts({
            QKeySequence(Qt::CTRL | Qt::Key_PageUp),
            QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Tab),
#if defined(AK_OS_MACOS)
            QKeySequence(Qt::META | Qt::SHIFT | Qt::Key_Tab),
            QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_BracketLeft),
            QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_Left),
#endif
        });
        QObject::connect(m_open_previous_tab_action, &QAction::triggered, this, []() {
            if (auto* window = Application::the().active_window_if_any())
                window->open_previous_tab();
        });

        m_search_tabs_action = make_action("&Search Tabs…");
        m_search_tabs_action->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_A));
        QObject::connect(m_search_tabs_action, &QAction::triggered, this, []() {
            if (auto* window = Application::the().active_window_if_any())
                window->show_all_tabs();
        });

        m_move_focus_action = make_action("Move &Focus Between Toolbar and Page");
        m_move_focus_action->setShortcut(QKeySequence(Qt::Key_F6));
        QObject::connect(m_move_focus_action, &QAction::triggered, this, []() {
            if (auto* window = Application::the().active_window_if_any())
                window->move_focus_between_chrome_and_page();
        });

        m_open_file_action = make_action("&Open File…");
        m_open_file_action->setShortcut(QKeySequence(QKeySequence::StandardKey::Open));
        QObject::connect(m_open_file_action, &QAction::triggered, this, []() { Application::the().open_file(); });

        m_open_location_action = make_action("Open &Location…");
        m_open_location_action->setShortcuts({ QKeySequence(Qt::CTRL | Qt::Key_L), QKeySequence(Qt::ALT | Qt::Key_D) });
        QObject::connect(m_open_location_action, &QAction::triggered, this, []() {
            if (auto* tab = Application::the().active_tab())
                tab->focus_location_editor();
        });

        m_open_settings_action = create_application_action(*m_application_widget, application.open_settings_page_action(), IncludeActionIcon::No);
        m_open_downloads_action = create_application_action(*m_application_widget, application.open_downloads_page_action(), IncludeActionIcon::No);

        m_toggle_bookmark_bar_action = create_application_action(*m_application_widget, Application::bookmarks().toggle_bookmark_bar_action(), IncludeActionIcon::No);
        m_toggle_sidebar_action = create_application_action(*m_application_widget, application.toggle_vertical_tabs_expanded_action(), IncludeActionIcon::No);

        m_reload_action = create_application_action(*m_application_widget, application.reload_action(), IncludeActionIcon::No);
        m_reload_bypassing_cache_action = create_application_action(*m_application_widget, application.reload_bypassing_cache_action(), IncludeActionIcon::No);

        // NB: Esc only labels the item. The page view stops a load on Esc itself, after the node picker has had its
        //     chance at the key, so it isn't a window-wide shortcut that would take Esc from every widget.
        m_stop_action = make_action("&Stop");
        m_stop_action->setShortcut(QKeySequence(Qt::Key_Escape));
        m_stop_action->setShortcutContext(Qt::WidgetShortcut);
        QObject::connect(m_stop_action, &QAction::triggered, this, []() {
            if (auto* tab = Application::the().active_tab(); tab && tab->view().is_loading())
                tab->view().stop_loading();
        });

        m_find_in_page_action = make_action("&Find in Page…");
        m_find_in_page_action->setShortcuts(QKeySequence::keyBindings(QKeySequence::StandardKey::Find));
        QObject::connect(m_find_in_page_action, &QAction::triggered, this, []() {
            if (auto* window = Application::the().active_window_if_any())
                window->show_find_in_page();
        });

        m_find_next_action = make_action("Find &Next");
        m_find_next_action->setShortcuts(QKeySequence::keyBindings(QKeySequence::StandardKey::FindNext));
        QObject::connect(m_find_next_action, &QAction::triggered, this, []() {
            if (auto* tab = Application::the().active_tab())
                tab->find_next();
        });

        m_find_previous_action = make_action("Find Pre&vious");
        m_find_previous_action->setShortcuts(QKeySequence::keyBindings(QKeySequence::StandardKey::FindPrevious));
        QObject::connect(m_find_previous_action, &QAction::triggered, this, []() {
            if (auto* tab = Application::the().active_tab())
                tab->find_previous();
        });

        m_zoom_in_action = create_application_action(*m_application_widget, application.zoom_in_action(), IncludeActionIcon::No);
        m_zoom_out_action = create_application_action(*m_application_widget, application.zoom_out_action(), IncludeActionIcon::No);
        m_reset_zoom_action = create_application_action(*m_application_widget, application.reset_zoom_action(), IncludeActionIcon::No);

        m_quit_action = make_action("&Quit");
        m_quit_action->setShortcuts(QKeySequence::keyBindings(QKeySequence::StandardKey::Quit));
#if defined(AK_OS_MACOS)
        QObject::connect(m_quit_action, &QAction::triggered, this, []() { Application::the().quit(); });
#else
        QObject::connect(m_quit_action, &QAction::triggered, this, []() {
            // If there are no active windows, we are certainly already in the process of quitting.
            if (auto* window = Application::the().active_window_if_any())
                window->close();
        });
#endif

        m_bookmarks_menu = create_application_menu(*m_application_widget, Application::bookmarks().menu());

        m_history_menu = create_application_menu(*m_application_widget, application.history_menu());
        QObject::connect(m_history_menu, &QMenu::aboutToShow, this, [this]() {
            auto* tab = Application::the().active_tab();
            update_history_menu(*m_history_menu, tab ? &tab->view() : nullptr);
        });

        m_inspect_menu = create_application_menu(*m_application_widget, application.inspect_menu());
        m_debug_menu = create_application_menu(*m_application_widget, application.debug_menu());

        m_help_menu = new QMenu("Help", m_application_widget);
        m_help_menu->addAction(create_application_action(*m_application_widget, application.report_website_issue_action(), IncludeActionIcon::No));
        m_help_menu->addSeparator();
        m_help_menu->addAction(create_application_action(*m_application_widget, application.open_about_page_action(), IncludeActionIcon::No));
    }

#if defined(AK_OS_MACOS)
    void create_application_menu_bar()
    {
        m_menu_bar = Application::the().create_application_menu_bar(Application::ForBrowserWindow::No);
    }

    void create_dock_menu()
    {
        if (m_dock_menu)
            return;

        m_dock_menu = new QMenu(m_menu_bar);
        QObject::connect(m_dock_menu, &QMenu::aboutToShow, this, [this] {
            rebuild_dock_menu();
        });
        rebuild_dock_menu();
        m_dock_menu->setAsDockMenu();
    }
#endif

    void update_reopen_recently_closed_action()
    {
        if (!m_reopen_recently_closed_tab_action)
            return;

        m_reopen_recently_closed_tab_action->setText("&Reopen Closed Tab");
        auto* active_window = Application::the().active_window_if_any();
        auto& session = active_window ? active_window->session() : Application::session_store(WebView::IsPrivate::No);
        m_reopen_recently_closed_tab_action->setEnabled(session.has_closed_units());
    }

    void rebuild_bookmarks_menu()
    {
        if (m_bookmarks_menu)
            repopulate_application_menu(*m_bookmarks_menu, Application::bookmarks().menu());
    }

private:
    friend Application;

#if defined(AK_OS_MACOS)
    void rebuild_dock_menu()
    {
        m_dock_menu->clear();

        auto* new_window_action = m_dock_menu->addAction("New Window");
        QObject::connect(new_window_action, &QAction::triggered, m_dock_menu, [] {
            Application::the().open_new_window(WebView::IsPrivate::No);
        });

        auto* new_private_window_action = m_dock_menu->addAction("New Private Window");
        QObject::connect(new_private_window_action, &QAction::triggered, m_dock_menu, [] {
            Application::the().open_new_window(WebView::IsPrivate::Yes);
        });

        bool added_window_separator = false;
        for (auto* widget : QApplication::topLevelWidgets()) {
            auto* window = as_if<BrowserWindow>(widget);
            if (!window)
                continue;

            if (!added_window_separator) {
                m_dock_menu->addSeparator();
                added_window_separator = true;
            }

            auto title = window->windowTitle();
            title.replace("&", "&&");
            auto* action = m_dock_menu->addAction(title);
            action->setCheckable(true);
            action->setChecked(window == Application::the().active_window_if_any());

            QPointer<BrowserWindow> window_pointer = window;
            QObject::connect(action, &QAction::triggered, m_dock_menu, [window = window_pointer] {
                if (!window)
                    return;

                if (window->isMinimized())
                    window->showNormal();
                else
                    window->show();

                window->raise();
                window->activateWindow();
                Application::the().set_active_window(*window);
            });
        }
    }
#endif

    // The application actions require a QWidget owner, which we don't have at the time of creation. We use this dummy
    // widget instead, which we also can't properly parent as it would need a QWidget owner as well.
    OwnPtr<QWidget> m_application_widget { nullptr };

    PlainTextEventFilter m_plain_text_event_filter;

#if defined(AK_OS_MACOS)
    QMenuBar* m_menu_bar { nullptr };
    QMenu* m_dock_menu { nullptr };
#endif

    QMenu* m_bookmarks_menu { nullptr };
    QMenu* m_history_menu { nullptr };
    QMenu* m_inspect_menu { nullptr };
    QMenu* m_debug_menu { nullptr };
    QMenu* m_help_menu { nullptr };

    QAction* m_new_tab_action { nullptr };
    QAction* m_new_window_action { nullptr };
    QAction* m_new_private_window_action { nullptr };
    QAction* m_reopen_recently_closed_tab_action { nullptr };
    QAction* m_close_current_tab_action { nullptr };
    QAction* m_open_next_tab_action { nullptr };
    QAction* m_open_previous_tab_action { nullptr };
    QAction* m_search_tabs_action { nullptr };
    QAction* m_move_focus_action { nullptr };
    QAction* m_open_file_action { nullptr };
    QAction* m_open_settings_action { nullptr };
    QAction* m_open_downloads_action { nullptr };
    QAction* m_toggle_bookmark_bar_action { nullptr };
    QAction* m_toggle_sidebar_action { nullptr };
    QAction* m_open_location_action { nullptr };
    QAction* m_reload_action { nullptr };
    QAction* m_reload_bypassing_cache_action { nullptr };
    QAction* m_stop_action { nullptr };
    QAction* m_find_next_action { nullptr };
    QAction* m_find_previous_action { nullptr };
    QAction* m_find_in_page_action { nullptr };
    QAction* m_zoom_in_action { nullptr };
    QAction* m_zoom_out_action { nullptr };
    QAction* m_reset_zoom_action { nullptr };
    QAction* m_quit_action { nullptr };
};

#if defined(AK_OS_MACOS)
// Qt stores a MIME type it has no converter for under a private type that only Qt applications can read. This puts PNG
// data on the pasteboard as public.png, for other applications to paste. The bytes pass through undecoded, as they may
// come from a page.
class PNGMimeConverter final : public QUtiMimeConverter {
public:
    virtual QString utiForMime(QString const& mime) const override
    {
        return mime == "image/png" ? QStringLiteral("public.png") : QString {};
    }

    virtual QString mimeForUti(QString const& uti) const override
    {
        return uti == "public.png" ? QStringLiteral("image/png") : QString {};
    }

    virtual QVariant convertToMime(QString const&, QList<QByteArray> const& data, QString const&) const override
    {
        return data.isEmpty() ? QVariant {} : QVariant { data.first() };
    }

    virtual QList<QByteArray> convertFromMime(QString const&, QVariant const& data, QString const&) const override
    {
        return { data.toByteArray() };
    }
};
#endif

Application* Application::s_the = nullptr;

ErrorOr<NonnullOwnPtr<Application>> Application::create(Main::Arguments const& arguments)
{
    auto application = adopt_own(*new Application);
    application->m_command_line = MUST(String::join(' ', arguments.strings));

#ifdef LADYBIRD_HUB_IN_OWN_PROCESS
    WebView::platform_init();
    application->m_arguments_for_qt = arguments;
    application->will_create_event_loop(application->m_arguments_for_qt);
    application->m_event_loop = &Core::EventLoop::initialize_for_current_thread();
    application->did_create_event_loop(*application->m_event_loop);
    TRY(application->connect_to_hub(arguments));
#else
    application->m_hub = TRY(Hub::Application::create(arguments, static_cast<Hub::ApplicationHost*>(application.ptr())));

    // Headless mode has no UI to connect to the Hub.
    if (browser_options().headless_mode.has_value())
        return application;

    auto transports = TRY(IPC::TransportHandle::create_pair());
    application->m_connection_from_ui = Hub::ConnectionFromUI::construct(TRY(transports.first.create_transport()));
    application->m_hub_connection = adopt_ref(*new HubConnection(TRY(transports.second.create_transport())));
#endif
    return application;
}

#ifdef LADYBIRD_HUB_IN_OWN_PROCESS
#    if defined(AK_OS_MACOS)
// What the Hub gets its end of the connection from. The thread that watches for the Hub to end can outlive connecting.
struct HubBootstrap : public AtomicRefCounted<HubBootstrap> {
    IPC::TransportBootstrapMachServer server;
};
#    endif

ErrorOr<ByteString> Application::hub_executable_path()
{
    for (auto const& path : TRY(WebView::get_paths_for_helper_process("Hub"sv))) {
        if (!Core::System::access(path, X_OK).is_error())
            return path;
    }
    return Error::from_string_literal("Unable to find the Hub");
}

Hub::UIInfo Application::current_ui_info() const
{
    Hub::UIInfo info;
    info.ui_font_family = ui_font_family();
    info.system_font_family = system_font_family();
    info.interface_color_scheme = interface_color_scheme();
    info.supports_picture_in_picture = supports_picture_in_picture();
    info.has_location_source = Core::GeolocationProvider::is_available();
    // NB: The Hub reads this before it first asks for a position, which is long before the UI's provider could tell it.
    if (info.has_location_source) {
        if (auto provider = Core::GeolocationProvider::create(); !provider.is_error())
            info.location_authorization = provider.value()->authorization();
    }
    info.has_translator = has_translator();
    info.has_spell_checker = m_spell_checker != nullptr;
    info.spell_checker_learns_for_every_tag = m_spell_checker && m_spell_checker->learns_for_every_tag();
    info.spell_checker_can_suggest = m_spell_checker && m_spell_checker->can_suggest();
    if (m_microphone_authorizer)
        info.microphone_authorization = m_microphone_authorizer->authorization();
    info.supports_selection_clipboard = supports_clipboard_type(ClipboardType::Selection);
    info.supports_system_menu_bar = supports_system_menu_bar();
    info.supports_vertical_tabs = supports_vertical_tabs();
    info.supports_private_browsing_windows = supports_private_browsing_windows();
    info.supports_client_side_window_decorations = supports_client_side_window_decorations();
    info.reports_scroll_momentum = platform_reports_scroll_momentum();
    info.scrolls_elastically = platform_scrolls_elastically();
    info.scrollbar_track_press_jumps = platform_scrollbar_track_press_jumps();
    info.preferred_languages = platform_preferred_languages();
    info.presentation_gpu_device = platform_presentation_gpu_device();
    info.presentation_drm_format_modifiers = platform_presentation_drm_format_modifiers();
    return info;
}

ErrorOr<void> Application::connect_to_hub(Main::Arguments const& arguments)
{
    auto hub_path = TRY(hub_executable_path());

#    if defined(LADYBIRD_HAS_TRANSLATOR)
    m_translator = create_platform_translator();
    // The Hub decides what to offer for each page, so it hears whenever a language's download starts or finishes.
    if (m_translator) {
        m_translator->on_languages_changed = [] {
            if (auto* hub = HubConnection::the_if_connected())
                hub->async_translator_languages_changed();
        };
    }
#    endif
#    if defined(AK_OS_MACOS)
    m_spell_checker = create_platform_spell_checker();
    m_microphone_authorizer = make<MicrophoneAuthorizerMacOS>();
#    endif

    IPC::MessageBuffer ui_info_buffer;
    IPC::Encoder encoder { ui_info_buffer };
    TRY(encoder.encode(current_ui_info()));

    Vector<ByteString> hub_arguments;
    for (auto argument : arguments.strings.slice(1))
        hub_arguments.append(argument);
#    if defined(AK_OS_MACOS)
    // NB: When macOS launches the browser to open a URL or a file, such as a link clicked in another app, the URL is in a
    //     launch Apple event to this process, never in argv. The Hub opens it like a URL from the command line, and
    //     hands it to the browser already running for the profile, if there is one.
    hub_arguments.extend(take_urls_from_launch_apple_events());
#    endif
    hub_arguments.append(ByteString::formatted("--ui-info={}", encode_hex(ui_info_buffer.data().span())));

    Core::ProcessSpawnOptions spawn_options {
        .name = "Hub"sv,
        .executable = hub_path,
        .arguments = hub_arguments,
    };

#    if defined(AK_OS_MACOS)
    // NB: This must answer while the main thread waits for the Hub to initialize, so it answers on its own thread.
    auto hub_bootstrap = adopt_ref(*new HubBootstrap);
    IPC::MachBootstrapListener bootstrap_listener { WebView::mach_server_name_for_process("Ladybird-UI"sv, Core::System::getpid()) };
    if (!bootstrap_listener.is_initialized())
        return Error::from_string_literal("Unable to start the Mach server for the Hub");
    bootstrap_listener.on_bootstrap_request = [hub_bootstrap](IPC::MachBootstrapListener::BootstrapRequest request) {
        if (auto result = hub_bootstrap->server.handle_bootstrap_request(request.pid, move(request.reply_port)); result.is_error())
            dbgln("Unable to bootstrap the Hub: {}", result.error());
    };
    hub_arguments.append(ByteString::formatted("--ui-mach-server={}", bootstrap_listener.server_port_name()));
    spawn_options.arguments = hub_arguments;

    auto port_a_recv = TRY(Core::MachPort::create_with_right(Core::MachPort::PortRight::Receive));
    auto port_a_send = TRY(port_a_recv.insert_right(Core::MachPort::MessageRight::MakeSend));
    auto port_b_recv = TRY(Core::MachPort::create_with_right(Core::MachPort::PortRight::Receive));
    auto port_b_send = TRY(port_b_recv.insert_right(Core::MachPort::MessageRight::MakeSend));

    // The Hub may receive startup messages before it has constructed its transport.
    IPC::TransportMachPort::raise_receive_queue_limit(port_b_recv);

    Optional<Core::Process> spawned_process;
    {
        MutexLocker child_registration_locker(hub_bootstrap->server.child_registration_lock());
        spawned_process = TRY(Core::Process::spawn(spawn_options));
        hub_bootstrap->server.register_child_transport(spawned_process->pid(), IPC::TransportBootstrapMachPorts { move(port_b_recv), move(port_a_send) });
    }
    auto process = spawned_process.release_value();

    // NB: Until the Hub takes its ports, we hold them ourselves, so nothing would tell us of a Hub that ended before
    //     that. Letting go of them then closes the connection, as a socket does when its peer ends.
    auto hub_exit_watcher = Threading::Thread::construct("HubExitWatcher"sv, [hub_bootstrap, pid = process.pid()]() -> intptr_t {
        siginfo_t info {};
        while (waitid(P_PID, pid, &info, WEXITED | WNOWAIT) < 0 && errno == EINTR)
            ;
        hub_bootstrap->server.unregister_child_transport(pid);
        return 0;
    });
    hub_exit_watcher->start();
    hub_exit_watcher->detach();

    auto transport = make<IPC::Transport>(move(port_a_recv), move(port_b_send));
#    else
    int socket_fds[2] {};
    TRY(Core::System::socketpair(AF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC, 0, socket_fds));

    spawn_options.file_actions.append(Core::FileAction::DupFd { .write_fd = socket_fds[1], .fd = socket_fds[1] });
    TRY(Core::Environment::set("SOCKET_TAKEOVER"sv, ByteString::formatted("ipc:{}", socket_fds[1]), Core::Environment::Overwrite::Yes));
    auto process = TRY(Core::Process::spawn(spawn_options));
    TRY(Core::Environment::unset("SOCKET_TAKEOVER"sv));
    TRY(Core::System::close(socket_fds[1]));

    auto ipc_socket = TRY(Core::LocalSocket::adopt_fd(socket_fds[0]));
    TRY(ipc_socket->set_blocking(true));
    auto transport = make<IPC::Transport>(move(ipc_socket));
#    endif
    m_hub_connection = adopt_ref(*new HubConnection(move(transport)));

    // The Hub can find that there is nothing for us to do, as when another process has this profile and takes our URLs,
    // or when asked for its usage. It ends right away then, and how it ended is how we end.
    auto initial_state = m_hub_connection->try_initialize();
    if (initial_state.is_error()) {
        m_exit_status_if_done = TRY(process.wait_for_termination());
        return {};
    }
    auto state = initial_state.release_value();
    m_hub_connection->did_initialize();

    m_settings = make<WebView::Settings>(WebView::Settings::create_from_json(TRY(JsonValue::from_string(state.settings_json))));
    m_bookmark_store = make<BookmarkStore>(BookmarkStore::create(LexicalPath::join(state.config_directory, "Bookmarks.json"sv).string()));
    create_session_store(state.data_directory, state.persists_sessions);
    create_history_store(state.data_directory, state.uses_sql_database);
    // NB: Reports stay in files, which both of us read. This process keeps its own for when it crashes, and the next
    //     launch recovers that the same as one of the Hub's.
    m_browser_options.disable_sandbox = state.sandbox_is_disabled ? WebView::DisableSandbox::Yes : WebView::DisableSandbox::No;
    WebView::CrashReport::set_sandbox_is_disabled(m_browser_options.disable_sandbox);
    auto& crash_report_store = WebView::CrashReportStore::the();
    crash_report_store.set_directory(move(state.crash_report_directory));
    if (auto result = crash_report_store.initialize_browser_crash_handler(); result.is_error())
        warnln("Could not prepare UI crash reporting: {}", result.error());

    m_browser_options.urls = move(state.urls);
    m_browser_options.raw_urls = move(state.raw_urls);
    m_browser_options.devtools_port = state.devtools_port;
#    if defined(AK_OS_MACOS)
    set_profile_application_icon(state.profile_identifier, state.profile_identity, state.profile_is_temporary);
#    endif
    m_application_actions = make<ApplicationActions>(ApplicationActions::PageOptions {
        .user_agent = move(state.user_agent),
        .navigator_compatibility_mode = state.navigator_compatibility_mode,
        .scripting_enabled = state.scripting_enabled,
        .blocks_pop_ups = state.blocks_pop_ups,
        .devtools_enabled = state.devtools_enabled,
    });
    RemoteDownloads::the().set_downloads(state.downloads);

    create_platform_actions();

    WebView::ReportSubmission::set_post_function([](auto const& url, auto headers, auto body, auto on_response) -> Function<void()> {
        auto request_id = HubConnection::the().post_report(url, move(headers), move(body), [on_response = move(on_response)](auto response) {
            on_response({ .had_network_error = response.had_network_error, .response_code = response.response_code, .headers = move(response.headers), .body = move(response.body) });
        });
        return [request_id] {
            if (auto* hub = HubConnection::the_if_connected())
                hub->stop_request(request_id);
        };
    });

#    if defined(AK_OS_MACOS)
    // NB: The system tells of this through the run loop, and only this process has one.
    if (!Core::Environment::has("TZ"sv)) {
        m_time_zone_watcher = TRY(Core::TimeZoneWatcher::create());
        m_time_zone_watcher->on_time_zone_changed = [] {
            if (auto* hub = HubConnection::the_if_connected())
                hub->async_system_time_zone_changed();
        };
    }
#    endif

    // NB: The user can give the browser the microphone in the system settings at any time, and comes back to the browser
    //     after doing it.
    if (m_microphone_authorizer) {
        QObject::connect(qGuiApp, &QGuiApplication::applicationStateChanged, [this](Qt::ApplicationState state) {
            if (state != Qt::ApplicationActive)
                return;
            if (auto* hub = HubConnection::the_if_connected())
                hub->async_microphone_authorization_changed(m_microphone_authorizer->authorization());
        });
    }

    // NB: The UI info holds the interface color scheme, which changes with the interface theme as well as with the
    //     system's appearance. The views take it from the Hub only after this, as they update their palettes later.
    auto send_ui_info = [this] {
        if (auto* hub = HubConnection::the_if_connected())
            hub->async_ui_info_changed(current_ui_info());
    };
    QObject::connect(&SystemAppearance::the(), &SystemAppearance::changed, send_ui_info);
    QObject::connect(&SystemAppearance::the(), &SystemAppearance::interface_appearance_changed, send_ui_info);
    return {};
}

Vector<WebView::ProcessSnapshot> Application::process_snapshots()
{
    auto snapshots = HubConnection::the().process_snapshots();

    // NB: The Hub knows the processes that it launched. Only this one can always tell how it is doing itself.
    if (m_ui_process_statistics.processes.is_empty()) {
        auto process_info = make<Core::Platform::ProcessInfo>(Core::System::getpid());
#    if defined(AK_OS_MACH)
        mach_port_t task_name_port = MACH_PORT_NULL;
        VERIFY(task_get_special_port(mach_task_self(), TASK_NAME_PORT, &task_name_port) == KERN_SUCCESS);
        process_info->task_name_port = Core::MachPort::adopt_right(task_name_port, Core::MachPort::PortRight::Send);
#    endif
        m_ui_process_statistics.processes.append(move(process_info));
    }
    if (auto result = Core::Platform::update_process_statistics(m_ui_process_statistics); result.is_error())
        dbgln("Unable to update the UI's process statistics: {}", result.error());

    auto const& statistics = *m_ui_process_statistics.processes.first();
    WebView::ProcessSnapshot snapshot;
    snapshot.child_id = NumericLimits<u64>::max();
    snapshot.pid = statistics.pid;
    snapshot.type = WebView::ProcessType::UI;
    snapshot.cpu_percent = statistics.cpu_percent;
    snapshot.memory_usage_bytes = statistics.memory_usage_bytes;
    snapshots.append(move(snapshot));
    return snapshots;
}

bool Application::has_translator() const { return m_translator != nullptr; }
Optional<int> Application::exit_status_if_done() const { return m_exit_status_if_done; }

ErrorOr<int> Application::execute()
{
    return m_event_loop->exec();
}
#else
bool Application::has_translator() const { return WebView::Application::the().translator() != nullptr; }
Optional<int> Application::exit_status_if_done() const
{
    if (!hub().should_exit_after_profile_coordination())
        return {};
    outln("Opening in existing process");
    return 0;
}
ErrorOr<int> Application::execute() { return hub().execute(); }
#endif

Application::Application()
{
    s_the = this;
}

Application::~Application()
{
    // NB: The application tears down its processes as it goes, which must happen after the QApplication is gone, and
    //     with nothing left here to call back into.
    ScopeGuard tear_down_hub = [this] {
#if !defined(AK_OS_WINDOWS)
        // No event loop runs the handlers during teardown, so a signal from here on takes its default action again.
        for (auto handler : m_session_signal_handlers)
            Core::EventLoop::unregister_signal(handler);
#endif

        // Only a normal exit gets this far, after every window is gone. A crash, or the Hub going away, ends this
        // process at once, so it never records a clean exit.
        if (m_session_store) {
            m_session_store->flush_dirty_state();
            m_session_store->mark_clean_exit();
        }

        // NB: The views go along with the windows that showed them, and their timers need the QApplication.
#ifndef LADYBIRD_HUB_IN_OWN_PROCESS
        if (m_hub)
            m_hub->destroy_all_views();
#endif
        m_application = nullptr;
        m_hub_connection = nullptr;
#ifndef LADYBIRD_HUB_IN_OWN_PROCESS
        m_connection_from_ui = nullptr;
        if (m_hub)
            m_hub->detach_from_host();
        m_hub = nullptr;
#endif
        s_the = nullptr;
    };

    // NB: A run stopped by a signal leaves its windows open for the next launch to recover. Their pages must still go
    //     before the QApplication does, since the processes torn down after it end their navigations, which updates
    //     their widgets.
    if (!m_application)
        return;
    Vector<QPointer<BrowserWindow>> windows;
    for (auto* widget : QApplication::topLevelWidgets()) {
        if (auto* window = as_if<BrowserWindow>(widget))
            windows.append(window);
    }
    for (auto& window : windows)
        delete window.data();

    // A Picture-in-Picture window is a widget too, so it must also go before the QApplication does.
    m_picture_in_picture_windows.clear();
}

void Application::show_process_manager()
{
    if (!m_process_manager_window) {
        m_process_manager_window = make<ProcessManagerWindow>(ProcessManagerWindow::Source {
#ifdef LADYBIRD_HUB_IN_OWN_PROCESS
            .take_snapshots = [this] { return process_snapshots(); },
            .end_process = [](u64 child_id) { return HubConnection::the().terminate_process(child_id); },
#else
            .take_snapshots = [] { return WebView::process_snapshots(WebView::Application::process_manager()); },
            .end_process = [](u64 child_id) { return WebView::end_process(WebView::Application::process_manager(), child_id); },
#endif
            .favicon_hash = [](WebView::ProcessPageSnapshot const& page) -> Optional<String> {
                if (auto* view = RemoteView::find(page.view_id))
                    return view->favicon_hash();
                return {};
            },
            .favicon_png = [](String const& favicon_hash) {
                // NB: A private window's favicons are in a store of their own, which the hash finds as well.
                if (auto png = favicon_store(WebView::IsPrivate::No).favicon_png(favicon_hash); png.has_value())
                    return png;
                return favicon_store(WebView::IsPrivate::Yes).favicon_png(favicon_hash); },
        });
#if defined(AK_OS_MACOS)
        // The traffic lights are centered in the Task Manager's title bar, which is 52 px tall.
        m_process_manager_window->on_title_bar_change = [window = m_process_manager_window.ptr()] {
            hide_appkit_window_title(*window);
            offset_appkit_window_controls(*window, 3, 10);
        };
        m_process_manager_window->on_title_bar_drag = [window = m_process_manager_window.ptr()] {
            return start_appkit_window_drag(*window);
        };
#endif
    }
    m_process_manager_window->show();
    m_process_manager_window->raise();
    m_process_manager_window->activateWindow();
}

void Application::create_platform_actions()
{
#ifndef LADYBIRD_HUB_IN_OWN_PROCESS
    m_settings = make<WebView::Settings>(WebView::Settings::create_from_json(WebView::Application::settings().serialize_json()));
    m_bookmark_store = make<BookmarkStore>(BookmarkStore::create(LexicalPath::join(WebView::Application::profile().paths().config, "Bookmarks.json"sv).string()));
    auto& hub = WebView::Application::the();
    create_session_store(WebView::Application::profile().paths().data, hub.persists_sessions());
    create_history_store(WebView::Application::profile().paths().data, WebView::Application::browser_options().disable_sql_database == WebView::DisableSQLDatabase::No);
    m_application_actions = make<ApplicationActions>(ApplicationActions::PageOptions {
        .user_agent = hub.user_agent_string(),
        .navigator_compatibility_mode = hub.navigator_compatibility_mode(),
        .scripting_enabled = hub.scripting_enabled(),
        .blocks_pop_ups = hub.blocks_pop_ups(),
        .devtools_enabled = hub.devtools_enabled(),
    });
    RemoteDownloads::the().set_downloads(Hub::ConnectionFromUI::downloads());
#endif
    if (auto* hub = HubConnection::the_if_connected())
        hub->set_settings_from_hub(m_settings->serialize_json());
    m_settings->on_persist = [](JsonValue const& settings) {
        if (auto* hub = HubConnection::the_if_connected())
            hub->send_settings_changes(settings);
    };
    m_web_ui_pages = make<WebUIPages>();
    m_bookmarks = make<Bookmarks>(*m_bookmark_store);
    m_web_ui_pages->register_page("history"_string, "History"_string, [](u64 web_ui_id, WebView::IsPrivate is_private) -> NonnullOwnPtr<WebUIPage> {
        return make<HistoryPage>(web_ui_id, is_private);
    });
    m_web_ui_pages->register_page("downloads"_string, "Downloads"_string, [](u64 web_ui_id, WebView::IsPrivate is_private) -> NonnullOwnPtr<WebUIPage> {
        return make<DownloadsPage>(web_ui_id, is_private);
    });
    m_web_ui_pages->register_page("version"_string, "Version"_string, [](u64 web_ui_id, WebView::IsPrivate) -> NonnullOwnPtr<WebUIPage> {
        return make<VersionPage>(web_ui_id);
    });

    m_open_task_manager_action = Action::create("Open Task Manager"sv, ActionID::OpenTaskManager, [this]() {
        show_process_manager();
    });
    m_application_actions->inspect_menu().add_action(*m_open_task_manager_action);

    if (m_application) {
        auto& qapplication = static_cast<LadybirdQApplication&>(*m_application);
        qapplication.create_application_actions();
#if defined(AK_OS_MACOS)
        qapplication.create_application_menu_bar();
        qapplication.create_dock_menu();
#endif
    }
}

void Application::will_create_event_loop(Main::Arguments& arguments)
{
    Core::EventLoopManager::install(*new EventLoopManagerQt);
#if !defined(Q_OS_MACOS) && !defined(Q_OS_WIN)
    // Prevent any competing AT-SPI2 bridge from attaching to our process. On GNOME/Fedora, Qt loads the libqgtk3
    // platform-theme plugin by default — which pulls in libgtk-3 via DT_NEEDED. Once libgtk-3 is loaded, its init
    // calls atk_bridge_adaptor_init(). And that registers an ATK app on the shared AT-SPI2 bus under *our* D-Bus
    // name, hijacking it from QSpiAccessibleBridge. AT clients then see a GTK app with zero accessible children
    // (because ATK has no widgets to expose in a Qt app) — and our entire accessibility tree is unreachable.
    //
    // So, we set NO_AT_BRIDGE=1 here — before libgtk-3 is loaded – to tell GTK's init code to skip the atk-bridge
    // registration. The library is still linked, but the hijack doesn't happen; AT-SPI2 queries reach Qt's bridge
    // (and therefore our WebContentViewAccessible) normally. Firefox's toolkit/xre/nsAppRunner.cpp does the same.
    qputenv("NO_AT_BRIDGE", "1");
#endif
    m_application = make<LadybirdQApplication>(arguments);
#if defined(AK_OS_MACOS)
    // NB: Qt owns its converters, and deletes them along with the QApplication.
    new PNGMimeConverter;
#endif
#if defined(AK_OS_LINUX)
    QGuiApplication::setDesktopFileName(QStringLiteral("org.ladybird.Ladybird"));
    Ladybird::clip_translucent_windows_that_cannot_blend();
#endif
#if !defined(Q_OS_MACOS) && !defined(Q_OS_WIN)
    install_orca_scripts();
#endif
}

void Application::did_create_event_loop(Core::EventLoop& event_loop)
{
#if defined(AK_OS_LINUX)
    Ladybird::initialize_external_url_handler(event_loop);
#endif
#if defined(AK_OS_MACOS)
    Ladybird::on_scrollbar_track_press_behavior_change([] {
        if (auto* hub = HubConnection::the_if_connected())
            hub->async_ui_info_changed(Application::the().current_ui_info());
    });
#endif
    static_cast<EventLoopImplementationQt&>(event_loop.impl()).set_main_loop();
}

Optional<String> Application::ui_font_family() const
{
    if (!m_application)
        return {};
    return ak_string_from_qstring(QGuiApplication::font().family());
}

// The widgets' palette follows the interface theme, and the system's appearance when the theme follows the system.
Web::CSS::PreferredColorScheme Application::interface_color_scheme() const
{
    return Theme::is_dark() ? Web::CSS::PreferredColorScheme::Dark : Web::CSS::PreferredColorScheme::Light;
}

// Bookmarking the page from the menu or its shortcut does what the star in the address field does.
void Application::show_bookmark_popover()
{
    if (auto* tab = active_tab())
        tab->show_bookmark_popover();
}

bool Application::platform_reports_scroll_momentum() const
{
    // Only Qt on macOS sends momentum scroll events.
    if (!m_application)
        return true;
    return QGuiApplication::platformName() == QStringLiteral("cocoa");
}

bool Application::platform_scrolls_elastically() const
{
    // Only Qt on macOS reports the end of every touchpad gesture, and there scrollers are expected to bounce.
    if (!m_application)
        return false;
    return QGuiApplication::platformName() == QStringLiteral("cocoa");
}

bool Application::platform_scrollbar_track_press_jumps() const
{
#if defined(AK_OS_MACOS)
    if (!m_application)
        return false;
    return scrollbar_track_press_jumps();
#else
    return false;
#endif
}

Vector<String> Application::platform_preferred_languages() const
{
    // The system's UI languages, most preferred first, as BCP 47 tags ("en-US", then "en").
    Vector<String> languages;
    for (auto const& language : QLocale::system().uiLanguages())
        languages.append(ak_string_from_qstring(language));
    return languages;
}

Optional<Gfx::GpuDeviceIdentity> Application::platform_presentation_gpu_device() const
{
#ifdef LADYBIRD_QT_USE_VULKAN_WINDOW
    return WebContentView::presentation_gpu_device();
#else
    return {};
#endif
}

Vector<u64> Application::platform_presentation_drm_format_modifiers() const
{
#ifdef LADYBIRD_QT_USE_VULKAN_WINDOW
    return WebContentView::presentation_drm_format_modifiers();
#else
    return {};
#endif
}

// On macOS, WebContent resolves system-ui with CoreText, so the system font family is not sent there.
Optional<String> Application::system_font_family() const
{
#if defined(AK_OS_MACOS)
    return {};
#else
    return ui_font_family();
#endif
}

BrowserWindow& Application::new_window(Vector<URL::URL> const& initial_urls, WindowConfiguration const& configuration, BrowserWindow::IsPopupWindow is_popup_window, WebView::IsPrivate is_private, Optional<u64> hub_view_id, ShowWindow show_window, Optional<Web::HTML::PreparedNavigationDescriptor> initial_navigation)
{
    auto* window = new BrowserWindow(initial_urls, is_popup_window, is_private, hub_view_id);

    auto should_focus_location_editor = initial_urls.size() == 1 && initial_urls.first() == Application::settings().new_tab_page_url();
    if (should_focus_location_editor) {
        if (auto* tab = window->current_tab())
            tab->set_url_is_hidden(true);
    }

    present_window(*window, configuration, show_window);

    size_t initial_url_index = 0;
    window->for_each_tab([&](Tab& tab) {
        if (initial_url_index >= initial_urls.size())
            return;
        if (initial_url_index++ == 0 && initial_navigation.has_value())
            tab.view().load(initial_navigation.release_value());
        else
            tab.navigate(initial_urls[initial_url_index - 1]);
    });

    if (should_focus_location_editor && show_window == ShowWindow::Yes) {
        QTimer::singleShot(0, window, [window] {
            if (auto* tab = window->current_tab())
                tab->focus_location_editor();
        });
    }
    return *window;
}

void Application::present_window(BrowserWindow& window, WindowConfiguration const& configuration, ShowWindow show_window)
{
    set_active_window(window);
    QObject::connect(&window, &QObject::destroyed, m_application.ptr(), [this, window = &window] {
        if (m_active_window == window)
            m_active_window = nullptr;
    });

    window.set_window_rect(configuration.x, configuration.y, configuration.width, configuration.height);
    if (show_window == ShowWindow::Yes) {
        if (configuration.maximized == true)
            window.showMaximized();
        else
            window.show();

        window.activateWindow();
        window.raise();
    }
}

SessionLaunchPolicy Application::launch_restore_policy() const
{
    // Without the setting, only pinned tabs come back; with it, every window and tab does.
    if (Application::settings().restore_previous_session())
        return SessionLaunchPolicy::Everything;
    return SessionLaunchPolicy::PinnedOnly;
}

void Application::create_session_store(ByteString const& data_directory, bool persists_sessions)
{
    auto session_store = [&]() -> ErrorOr<NonnullOwnPtr<SessionStore>> {
        if (!persists_sessions)
            return SessionStore::create();

        // Fall back without modifying the existing Sessions database.
        auto session_database = TRY(Database::Database::create(data_directory, "Sessions"sv, { .foreign_keys = Database::Database::ForeignKeys::Yes }));
        if (TRY(SessionStore::migrate_schema(session_database)) != Database::MigrationOutcome::Success)
            return Error::from_string_literal("Sessions database was created by a newer Ladybird version");
        return SessionStore::create(session_database, launch_restore_policy());
    }();
    if (session_store.is_error()) {
        dbgln("Sessions will not be persisted this session: {}", session_store.error());
        m_session_store = SessionStore::create();
    } else {
        m_session_store = session_store.release_value();
    }

    m_session_store->on_closed_units_changed = [this] {
        on_recently_closed_entries_changed();
    };

#if !defined(AK_OS_WINDOWS) && defined(LADYBIRD_HUB_IN_OWN_PROCESS)
    // A run stopped by SIGINT or SIGTERM ends like a quit, rather than like a crash: It saves the session, leaves its
    // windows for the next launch to recover, and records a clean exit. The Hub hears of a signal sent to it alone, and
    // passes it on.
    if (m_session_store->is_persistent()) {
        for (auto signal_number : { SIGINT, SIGTERM }) {
            m_session_signal_handlers.append(Core::EventLoop::register_signal(signal_number, [this](int) {
                quit_for_signal();
            }));
        }
    }
#endif
}

void Application::create_history_store(ByteString const& data_directory, bool uses_sql_database)
{
    auto create_autocomplete_service = [&](Optional<ByteString> history_database_directory) {
        m_autocomplete_service = make<WebView::AutocompleteService>(Core::EventLoop::current(), move(history_database_directory));
    };

    if (!uses_sql_database) {
        m_favicon_store = WebView::FaviconStore::create();
        m_history_store = WebView::HistoryStore::create_disabled();
        create_autocomplete_service({});
        return;
    }

    auto created = [&]() -> ErrorOr<bool> {
        // The History database is shared by the favicon and history stores. Preflight both before applying either
        // migration so a database that is too new remains untouched.
        auto database = TRY(Database::Database::create(data_directory, "History"sv));
        auto favicons_outcome = TRY(WebView::FaviconStore::migrate_schema(database, Database::MigrationMode::CheckOnly));
        auto history_outcome = TRY(WebView::HistoryStore::migrate_schema(database, Database::MigrationMode::CheckOnly));
        if (favicons_outcome != Database::MigrationOutcome::Success || history_outcome != Database::MigrationOutcome::Success)
            return false;
        if (TRY(WebView::FaviconStore::migrate_schema(database)) != Database::MigrationOutcome::Success)
            return false;
        if (TRY(WebView::HistoryStore::migrate_schema(database)) != Database::MigrationOutcome::Success)
            return false;
        m_favicon_store = TRY(WebView::FaviconStore::create(database));
        m_history_store = TRY(WebView::HistoryStore::create(database));
        return true;
    }();
    if (!created.is_error() && created.value()) {
        m_should_remove_unreferenced_favicons = true;
        // NB: The omnibox reads the history on a thread of its own, with a connection of its own.
        create_autocomplete_service(data_directory);
        return;
    }

    if (created.is_error())
        dbgln("Unable to open the History database: {}", created.error());
    else
        dbgln("History database was created by a newer Ladybird version; favicons and history will not be persisted this session");
    m_favicon_store = WebView::FaviconStore::create();
    m_history_store = WebView::HistoryStore::create(*m_favicon_store);
    create_autocomplete_service({});
}

Function<void()> Application::fetch_remote_suggestions(WebView::SearchEngine const& engine, String const& query, WebView::IsPrivate is_private, Function<void(RemoteResponse)> on_response)
{
    auto request_id = HubConnection::the().fetch_search_suggestions(engine.name, query, is_private,
        [on_response = move(on_response)](HubConnection::RequestResponse response) {
            auto content_type = HTTP::HeaderList::create(move(response.headers))->get("Content-Type"sv);
            on_response({
                .had_network_error = response.had_network_error,
                .response_code = response.response_code,
                .content_type = move(content_type),
                .body = MUST(ByteBuffer::copy(response.body.bytes())),
            });
        });
    return [request_id] {
        if (auto* hub = HubConnection::the_if_connected())
            hub->stop_request(request_id);
    };
}

WebView::FaviconStore& Application::favicon_store(WebView::IsPrivate is_private)
{
    if (is_private == WebView::IsPrivate::No)
        return *s_the->m_favicon_store;
    if (!s_the->m_private_favicon_store)
        s_the->m_private_favicon_store = WebView::FaviconStore::create();
    return *s_the->m_private_favicon_store;
}

// NB: The favicons of history and of bookmarks share a store, so it is only once the bookmarks are read that the
//     ones nothing uses any more are known.
void Application::remove_unreferenced_favicons(HashTable<String> referenced_hashes)
{
    if (!m_should_remove_unreferenced_favicons)
        return;
    m_should_remove_unreferenced_favicons = false;

    for (auto& hash : m_history_store->referenced_favicon_hashes())
        referenced_hashes.set(move(hash));
    m_favicon_store->remove_unreferenced_favicons(referenced_hashes);
}

SessionStore& Application::session_store(WebView::IsPrivate is_private)
{
    if (is_private == WebView::IsPrivate::No)
        return *s_the->m_session_store;

    // NB: Private windows keep nothing for the next run, and the next private session starts with nothing.
    if (!s_the->m_private_session_store)
        s_the->m_private_session_store = SessionStore::create();
    return *s_the->m_private_session_store;
}

void Application::did_clear_history_since(UnixDateTime since)
{
    m_history_store->remove_entries_accessed_since(since);
    for (auto* store : { m_session_store.ptr(), m_private_session_store.ptr() }) {
        if (!store)
            continue;
        if (auto result = store->remove_entries_accessed_since(since); result.is_error())
            dbgln("Unable to remove closed session entries: {}", result.error());
    }
    on_recently_closed_entries_changed();
}

void Application::quit_for_signal()
{
    if (m_quit_is_finishing)
        return;

    // The windows stay open in the session store, for the next launch to recover.
    // NB: QApplication::quit() would ask the windows to close first, which their pages may refuse. A signal does not
    //     wait for them, so this ends the event loop directly.
    m_quit_is_finishing = true;
    session_store(WebView::IsPrivate::No).application_quitting();
    Core::EventLoop::current().quit(0);
}

static WindowConfiguration window_configuration_for(SessionWindowGeometry const& geometry)
{
    QRect rect { geometry.x, geometry.y, geometry.width, geometry.height };

    // The display a window was on may be gone since the last run, so the window comes back on a screen that's there.
    auto* screen = QGuiApplication::screenAt(rect.center());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (screen) {
        auto available = screen->availableGeometry();
        rect.setSize(rect.size().boundedTo(available.size()));
        rect.moveLeft(clamp(rect.left(), available.left(), available.right() - rect.width() + 1));
        rect.moveTop(clamp(rect.top(), available.top(), available.bottom() - rect.height() + 1));
    }

    return {
        .x = Web::DevicePixels { rect.x() },
        .y = Web::DevicePixels { rect.y() },
        .width = Web::DevicePixels { rect.width() },
        .height = Web::DevicePixels { rect.height() },
        .maximized = geometry.maximized,
    };
}

BrowserWindow* Application::restore_previous_session(WindowConfiguration const& frontmost_window_configuration)
{
    auto plan = session_store(WebView::IsPrivate::No).take_launch_plan();

    BrowserWindow* frontmost_window = nullptr;
    for (size_t i = 0; i < plan.size(); ++i) {
        auto const& restored = plan[i];
        // The plan lists the frontmost window last, and opening the windows in that order leaves it on top.
        auto configuration = restored.geometry.has_value()
            ? window_configuration_for(*restored.geometry)
            : (i == plan.size() - 1 ? frontmost_window_configuration : configuration_for_new_window());

        auto* window = new BrowserWindow({}, BrowserWindow::IsPopupWindow::No, WebView::IsPrivate::No, {}, restored.window_id);
        window->restore_tabs(restored.tabs, restored.active_tab_index);
        present_window(*window, configuration, ShowWindow::Yes);
        frontmost_window = window;
    }
    return frontmost_window;
}

void Application::offer_to_restore_unexpectedly_closed_tabs(BrowserWindow& window)
{
    auto& store = session_store(WebView::IsPrivate::No);
    if (store.consecutive_unclean_exits() == 0)
        return;

    Vector<SessionWindowId> unit_ids;
    size_t tab_count = 0;
    for (auto const& unit : store.recently_closed_units(NumericLimits<size_t>::max())) {
        if (!unit.from_startup_recovery)
            continue;
        unit_ids.append(unit.id);
        tab_count += unit.tab_count;
    }
    if (unit_ids.is_empty() || !window.current_tab())
        return;

    window.current_tab()->show_restore_session_card(tab_count, [window = QPointer<BrowserWindow>(&window), unit_ids = AK::move(unit_ids)] {
        // A window that still shows only the new tab page it opened with is taken over by the tabs restored first.
        QPointer<Tab> new_tab_page;
        if (window && window->tab_count() == 1) {
            auto* tab = window->tab_at(0);
            if (tab->view().url() == Application::settings().new_tab_page_url() && !tab->view().navigate_back_action().enabled())
                new_tab_page = tab;
        }

        // NB: The units are listed most recently closed first, and reopening them oldest first keeps their order.
        for (size_t i = unit_ids.size(); i > 0; --i) {
            auto reopen_closed_window = new_tab_page && i == unit_ids.size() ? ReopenClosedWindow::InTargetWindow : ReopenClosedWindow::InNewWindow;
            Application::the().reopen_recently_closed_tab(window, unit_ids[i - 1], reopen_closed_window);
        }

        if (new_tab_page && window->tab_count() > 1)
            window->close_tab_without_keeping_it(window->tab_index(new_tab_page));
    });
}

void Application::open_startup_tabs(BrowserWindow& window, Vector<URL::URL> const& urls)
{
    auto const& new_tab_page_url = Application::settings().new_tab_page_url();
    for (size_t i = 0; i < urls.size(); ++i) {
        auto& tab = window.new_tab_from_url(urls[i], i == 0 ? Web::HTML::ActivateTab::Yes : Web::HTML::ActivateTab::No, BrowserWindow::TabLocation::end());
        if (urls.size() == 1 && urls[i] == new_tab_page_url) {
            tab.set_url_is_hidden(true);
            QTimer::singleShot(0, &window, [tab = QPointer<Tab>(&tab)] {
                if (tab)
                    tab->focus_location_editor();
            });
        }
    }
}

BrowserWindow* Application::non_private_window_if_any() const
{
    if (m_active_window && m_active_window->is_private() == WebView::IsPrivate::No)
        return m_active_window;

    for (auto* widget : QApplication::topLevelWidgets()) {
        if (auto* window = as_if<BrowserWindow>(widget); window && window->is_private() == WebView::IsPrivate::No)
            return window;
    }

    return nullptr;
}

WindowConfiguration Application::configuration_for_new_window() const
{
    if (auto* previous_active_window = active_window_if_any(); previous_active_window && previous_active_window->isVisible()) {
        return {
            .x = previous_active_window->pos().x() + new_window_cascade_offset,
            .y = previous_active_window->pos().y() + new_window_cascade_offset,
            .width = previous_active_window->width(),
            .height = previous_active_window->height(),
            .maximized = previous_active_window->isMaximized(),
        };
    }

    auto const& window_settings = Application::settings().window_settings();
    WindowConfiguration configuration {
        .width = window_settings.size.width(),
        .height = window_settings.size.height(),
        .maximized = window_settings.maximized,
    };
    if (window_settings.position.has_value()) {
        configuration.x = window_settings.position->x();
        configuration.y = window_settings.position->y();
    }
    return configuration;
}

void Application::open_new_tab()
{
    if (!m_active_window) {
        new_window({ Application::settings().new_tab_page_url() });
        return;
    }

    m_active_window->open_new_tab();
}

void Application::open_new_window(WebView::IsPrivate is_private)
{
    // FIXME: Create a new tab page specific to private windows.
    new_window({ Application::settings().new_tab_page_url() }, configuration_for_new_window(), BrowserWindow::IsPopupWindow::No, is_private);
}

void Application::restart_private_browsing_session()
{
    if (m_private_session_restart_in_progress || m_quit_in_progress)
        return;

    m_private_session_restart_in_progress = true;
    ask_next_window_to_close_for_private_session_restart();
}

void Application::ask_next_window_to_close_for_private_session_restart()
{
    if (!m_private_session_restart_in_progress)
        return;

    for (auto* widget : QApplication::topLevelWidgets()) {
        auto* window = as_if<BrowserWindow>(widget);
        if (!window || window->is_private() == WebView::IsPrivate::No || window->is_window_close_approved())
            continue;

        window->request_close_preflight([this](bool approved) {
            if (!approved) {
                cancel_private_session_restart();
                return;
            }
            QTimer::singleShot(0, m_application.ptr(), [this]() {
                ask_next_window_to_close_for_private_session_restart();
            });
        });

        return;
    }

    // Finish outside a page's reply, after checking again for windows opened or navigated during the preflight.
    QTimer::singleShot(0, m_application.ptr(), [this]() {
        finish_private_session_restart();
    });
}

void Application::finish_private_session_restart()
{
    if (!m_private_session_restart_in_progress)
        return;

    Vector<QPointer<BrowserWindow>> windows;
    for (auto* widget : QApplication::topLevelWidgets()) {
        auto* window = as_if<BrowserWindow>(widget);
        if (!window || window->is_private() == WebView::IsPrivate::No)
            continue;

        if (!window->is_window_close_approved()) {
            ask_next_window_to_close_for_private_session_restart();
            return;
        }

        windows.append(window);
    }

    HubConnection::the().async_reset_private_browsing_session();

    // Views pending deferred deletion may still report their history, and the replacement store reuses their ids.
    RemoteView::for_each([](RemoteView& view) {
        if (view.is_private() == WebView::IsPrivate::Yes)
            view.clear_session_tab_id();
    });
    m_private_session_store = nullptr;
    m_private_favicon_store = nullptr;
    open_new_window(WebView::IsPrivate::Yes);

    for (auto const& window : windows) {
        if (window)
            window->close();
    }

    m_private_session_restart_in_progress = false;
}

void Application::cancel_private_session_restart()
{
    if (!m_private_session_restart_in_progress)
        return;

    for (auto* widget : QApplication::topLevelWidgets()) {
        if (auto* window = as_if<BrowserWindow>(widget); window && window->is_private() == WebView::IsPrivate::Yes)
            window->cancel_window_close_preflight();
    }

    m_private_session_restart_in_progress = false;
}

void Application::focus_location_editor()
{
    if (!m_active_window) {
        new_window({ Application::settings().new_tab_page_url() });
        return;
    }

    if (auto* tab = m_active_window->current_tab())
        tab->focus_location_editor();
}

void Application::reopen_recently_closed_tab(BrowserWindow* target_window, Optional<SessionWindowId> unit_id, ReopenClosedWindow reopen_closed_window)
{
    auto is_private = target_window ? target_window->is_private() : WebView::IsPrivate::No;
    auto& session = target_window ? target_window->session() : session_store(WebView::IsPrivate::No);
    auto closed_unit = unit_id.has_value() ? session.take_closed_unit(*unit_id) : session.take_most_recently_closed();
    if (closed_unit.is_error()) {
        dbgln("Unable to reopen the most recently closed session unit: {}", closed_unit.error());
        return;
    }
    if (!closed_unit.value().has_value())
        return;

    auto unit = closed_unit.value().release_value();

    auto* window = target_window;
    bool restoring_to_source_window = false;
    if (!unit.was_window && unit.source_window_id.has_value()) {
        for (auto* widget : QApplication::topLevelWidgets()) {
            auto* candidate = as_if<BrowserWindow>(widget);
            if (!candidate || candidate->is_private() != is_private || candidate->session_window_id() != unit.source_window_id)
                continue;
            window = candidate;
            restoring_to_source_window = true;
            break;
        }
    }
    if ((unit.was_window && reopen_closed_window == ReopenClosedWindow::InNewWindow) || !window) {
        auto configuration = unit.geometry.has_value() ? window_configuration_for(*unit.geometry) : configuration_for_new_window();
        window = &new_window({}, configuration, BrowserWindow::IsPopupWindow::No, is_private);
    }

    Optional<int> next_insertion_index;
    if (restoring_to_source_window && unit.source_tab_index.has_value())
        next_insertion_index = static_cast<int>(clamp(*unit.source_tab_index, 0, static_cast<i64>(window->tab_count())));

    QPointer<Tab> tab_to_activate;
    i64 restored_tab_index = 0;
    for (auto& closed_tab : unit.tabs) {
        auto location = next_insertion_index.has_value() ? BrowserWindow::TabLocation::at_index(*next_insertion_index) : BrowserWindow::TabLocation::end();
        auto& tab = window->create_new_tab(Web::HTML::ActivateTab::No, location);
        if (next_insertion_index.has_value())
            ++*next_insertion_index;

        if (restored_tab_index == unit.active_tab_index)
            tab_to_activate = &tab;

        if (!closed_tab.history.has_value() || tab.view().restore_session_history_from_snapshot(closed_tab.history.release_value()).is_error())
            tab.navigate(closed_tab.active_url);

        if (closed_tab.pinned) {
            // Back in its own window, a pinned tab goes back to its place in the pinned group.
            Optional<int> pinned_index;
            if (restoring_to_source_window && unit.source_tab_index.has_value())
                pinned_index = static_cast<int>(*unit.source_tab_index);
            window->pin_tab(tab, pinned_index);
        }
        ++restored_tab_index;
    }

    // Pinning a reopened tab can move it, so its index is resolved only now.
    if (tab_to_activate) {
        if (auto index = window->tab_index(tab_to_activate); index >= 0)
            window->activate_tab(index);
    }

    update_reopen_recently_closed_actions();
}

void Application::crash_reports_changed()
{
    update_feedback_buttons();
}

void Application::update_feedback_buttons()
{
    for (auto* widget : QApplication::topLevelWidgets()) {
        if (auto* window = as_if<BrowserWindow>(widget))
            window->toolbar().update_feedback_button();
    }
}

void Application::open_file()
{
    if (!m_active_window) {
        auto filename = QFileDialog::getOpenFileUrl(nullptr, "Open file", QDir::homePath(), "All Files (*.*)");
        if (filename.isValid())
            new_window({ ak_url_from_qurl(filename) });
        return;
    }

    m_active_window->open_file();
}

static Vector<BrowserWindow*> visible_browser_windows_active_first(BrowserWindow* active_window)
{
    Vector<BrowserWindow*> windows;
    if (active_window && active_window->isVisible())
        windows.append(active_window);
    for (auto* widget : QApplication::topLevelWidgets()) {
        if (auto* window = as_if<BrowserWindow>(widget); window && window != active_window && window->isVisible())
            windows.append(window);
    }
    return windows;
}

void Application::quit()
{
    // A quit is already asking the pages, so a repeated one does nothing.
    if (m_quit_in_progress)
        return;

    if (m_private_session_restart_in_progress)
        cancel_private_session_restart();

    m_quit_in_progress = true;
    m_windows_awaiting_quit_preflight.clear();

    // The window the user is looking at gets asked first.
    for (auto* window : visible_browser_windows_active_first(m_active_window))
        m_windows_awaiting_quit_preflight.append(window);
    ask_next_window_to_close_for_quit();
}

void Application::ask_next_window_to_close_for_quit()
{
    while (!m_windows_awaiting_quit_preflight.is_empty()) {
        auto window = m_windows_awaiting_quit_preflight.take_first();
        if (!window)
            continue;

        window->request_close_preflight([this](bool approved) {
            if (approved)
                ask_next_window_to_close_for_quit();
            else
                cancel_quit();
        });
        return;
    }

    // The quit finishes on a later turn of the event loop, so that it never terminates the application from inside a
    // page's reply, or from inside the system's own request to quit.
    QTimer::singleShot(0, m_application.ptr(), [this] {
        finish_quit();
    });
}

bool Application::ask_windows_yet_to_agree_to_quit()
{
    for (auto* window : visible_browser_windows_active_first(m_active_window)) {
        if (!window->is_window_close_approved())
            m_windows_awaiting_quit_preflight.append(window);
    }
    if (m_windows_awaiting_quit_preflight.is_empty())
        return false;

    ask_next_window_to_close_for_quit();
    return true;
}

void Application::finish_quit()
{
    // A window can open while the quit asks the others, and a navigation or a closed tab withdraws a window's approval,
    // so any window that hasn't agreed yet gets asked before anything closes.
    if (ask_windows_yet_to_agree_to_quit())
        return;

    // Downloads are stopped only once every page has agreed, so a page that keeps the windows open leaves them running.
    if (!confirm_stop_active_downloads(active_window_if_any())) {
        cancel_quit();
        return;
    }

    // The downloads prompt runs an event loop of its own, in which windows can open or lose their approval too.
    if (ask_windows_yet_to_agree_to_quit())
        return;

    // The session store hears of the quit only once every window has agreed, so a canceled quit never reaches it. In
    // quitting mode, it keeps each window that closes as an open one, for the next launch to recover.
    m_quit_is_finishing = true;
    session_store(WebView::IsPrivate::No).application_quitting();

    // Each window's pages agreed, so it closes at once; the active window goes last, so no close activates another.
    auto windows = visible_browser_windows_active_first(m_active_window);
    for (auto* window : windows.in_reverse())
        window->close();

    QApplication::quit();
}

void Application::cancel_quit()
{
    m_quit_in_progress = false;
    m_windows_awaiting_quit_preflight.clear();

    // The windows whose pages already agreed go back to needing their own close check.
    for (auto* widget : QApplication::topLevelWidgets()) {
        if (auto* window = as_if<BrowserWindow>(widget))
            window->cancel_window_close_preflight();
    }

    // No activation was reported during the quit, and the window the quit left active gets no new WindowActivate while
    // it stays active, so it's reported here as the most recent window.
    if (m_active_window && m_active_window->isActiveWindow())
        m_active_window->report_activation_to_session_store();
}

bool Application::confirm_stop_active_downloads(QWidget* parent)
{
    auto& downloader = RemoteDownloads::the();

    if (downloader.has_unresumable_downloads()) {
        QMessageBox dialog(parent ? parent : active_window_if_any());
        dialog.setWindowTitle("Ladybird");
        dialog.setIcon(QMessageBox::Warning);
        dialog.setText("Downloads are still in progress.");
        dialog.setInformativeText("Quitting will cancel downloads that cannot be resumed.");
        auto* quit_button = dialog.addButton("Quit and Cancel Downloads", QMessageBox::DestructiveRole);
        dialog.addButton(QMessageBox::Cancel);
        dialog.setDefaultButton(QMessageBox::Cancel);
        dialog.exec();

        if (dialog.clickedButton() != quit_button)
            return false;

        downloader.cancel_unresumable_downloads();
    }

    downloader.pause_active_downloads();
    return true;
}

void Application::open_picture_in_picture_window(u64 view_id, u64 owner_view_id, Gfx::IntSize video_size)
{
    auto* owner_view = RemoteView::find(owner_view_id);
    if (!owner_view) {
        // NB: The owner went away before this arrived. The Hub must still hear that the window closed, or it would keep
        //     the window and its view.
        if (auto* hub = HubConnection::the_if_connected())
            hub->async_pip_did_close(view_id, false);
        return;
    }
    auto window = Ladybird::create_picture_in_picture_window(*owner_view, view_id, video_size);
    window->on_resize = [view_id](Gfx::IntSize size) {
        if (auto* hub = HubConnection::the_if_connected())
            hub->async_pip_did_resize(view_id, size);
    };
    window->on_close = [view_id](auto video_keeps_playing) {
        if (auto* hub = HubConnection::the_if_connected())
            hub->async_pip_did_close(view_id, video_keeps_playing == WebView::PictureInPictureWindow::VideoKeepsPlaying::Yes);
    };
    m_picture_in_picture_windows.set(view_id, move(window));
}

void Application::hide_picture_in_picture_window(u64 view_id)
{
    if (auto* window = m_picture_in_picture_windows.get(view_id).value_or(nullptr))
        window->hide();
}

void Application::set_picture_in_picture_video_size(u64 view_id, Gfx::IntSize video_size)
{
    if (auto* window = m_picture_in_picture_windows.get(view_id).value_or(nullptr))
        window->set_video_size(video_size);
}

void Application::close_picture_in_picture_window(u64 view_id)
{
    // NB: A window holds the view, which lets the Hub's view go as it goes. With no window, nothing here ever held it.
    if (m_picture_in_picture_windows.remove(view_id))
        return;
    if (auto* hub = HubConnection::the_if_connected())
        hub->async_destroy_view(view_id);
}

// The window that shows this view in one of its tabs, if any.
static BrowserWindow* window_for_view(RemoteView const& view)
{
    for (auto* widget : QApplication::topLevelWidgets()) {
        if (auto* window = as_if<BrowserWindow>(widget); window && window->tab_for_view(view))
            return window;
    }
    return nullptr;
}

OwnPtr<WebView::NotificationBackend> Application::create_notification_backend()
{
#if defined(AK_OS_MACOS)
    return make<WebView::NotificationBackendMacOS>();
#elif defined(AK_OS_LINUX) || defined(AK_OS_FREEBSD)
    return make<WebView::NotificationBackendFreedesktop>();
#else
    return {};
#endif
}

void Application::focus_view_from_notification(u64 view_id)
{
    auto* view = RemoteView::find(view_id);
    if (!view)
        return;
    auto* window = window_for_view(*view);
    if (!window)
        return;
    window->activate_tab(window->tab_index(window->tab_for_view(*view)));
    window->bring_to_front();
}

void Application::open_url_in_new_tab_from_notification(URL::URL const& url, Optional<u64> view_id, WebView::IsPrivate is_private)
{
    BrowserWindow* window = nullptr;
    if (view_id.has_value()) {
        if (auto* view = RemoteView::find(*view_id))
            window = window_for_view(*view);
    }
    // The site opens in the notification's own session: a private window's notification never lands in a normal
    // window, nor a normal one in a private window.
    if (window && window->is_private() != is_private)
        window = nullptr;
    if (!window && m_active_window && m_active_window->is_private() == is_private)
        window = m_active_window;
    if (!window) {
        for (auto* widget : QApplication::topLevelWidgets()) {
            if (auto* candidate = qobject_cast<BrowserWindow*>(widget); candidate && candidate->is_private() == is_private) {
                window = candidate;
                break;
            }
        }
    }
    if (!window) {
        new_window({ url }, configuration_for_new_window(), BrowserWindow::IsPopupWindow::No, is_private);
        return;
    }
    window->new_tab_from_url(url, Web::HTML::ActivateTab::Yes, BrowserWindow::TabLocation::end());
    window->bring_to_front();
}

Vector<BookmarkItem::Bookmark> Application::bookmarks_for_all_tabs_in_active_window() const
{
    auto* window = m_active_window;
    if (!window)
        return {};

    // Every tab gets a bookmark, so this walks the tabs directly: for_each_tab() skips the ones that haven't loaded.
    Vector<BookmarkItem::Bookmark> bookmarks;
    for (int i = 0; i < window->tab_count(); ++i) {
        auto& tab = *window->tab_at(i);
        if (tab.is_loaded()) {
            auto const& view = tab.view();
            bookmarks.append(BookmarkItem::Bookmark {
                .url = view.url(),
                .title = view.title().is_empty() ? Optional<String> {} : view.title().to_utf8(),
                .favicon_hash = view.favicon_hash(),
            });
            continue;
        }

        // A tab that hasn't loaded shows the title browsing history has for its page, or else its URL, which isn't a
        // title worth keeping. The bookmark store fills in the favicon on a later visit.
        auto url = tab.url();
        auto title = tab.title();
        auto has_title = !title.isEmpty() && title != qstring_from_ak_string(url.serialize());
        bookmarks.append(BookmarkItem::Bookmark {
            .url = url,
            .title = has_title ? ak_string_from_qstring(title) : Optional<String> {},
            .favicon_hash = {},
        });
    }
    return bookmarks;
}

Tab* Application::activate_tab_with_url(URL::URL const& url, URL::ExcludeFragment exclude_fragment) const
{
    auto is_private = m_active_window ? m_active_window->is_private() : WebView::IsPrivate::No;
    for (auto* window : visible_browser_windows_active_first(m_active_window)) {
        if (window->is_private() != is_private)
            continue;

        auto* tab = window->activate_tab_with_url(url, exclude_fragment);
        if (!tab)
            continue;

        if (window->isMinimized())
            window->setWindowState(window->windowState() & ~Qt::WindowMinimized);
        window->raise();
        window->activateWindow();
        return tab;
    }
    return nullptr;
}

void Application::activate_or_open_tab_with_url(URL::URL const& url)
{
    if (auto* tab = activate_tab_with_url(url, URL::ExcludeFragment::Yes))
        tab->view().load(url);
    else
        open_url_in_new_tab(url, Web::HTML::ActivateTab::Yes, {}, m_active_window ? m_active_window->is_private() : WebView::IsPrivate::No);
}

BrowserWindow* Application::window_of_view(u64 view_id) const
{
    auto* view = RemoteView::find(view_id);
    if (!view)
        return nullptr;
    for (auto* window : visible_browser_windows_active_first(m_active_window)) {
        if (window->tab_for_view(*view))
            return window;
    }
    return nullptr;
}

// NB: The window that is active now may not be the one the Hub saw. The new tab belongs with the page that opened it,
//     and if that window is gone, in a window of the same privacy: a view keeps the cookies and history it was made
//     with, whatever the window shows, and a URL from a private page must not end up in a normal session.
BrowserWindow* Application::window_for_new_tab(Optional<u64> source_view_id, WebView::IsPrivate is_private) const
{
    if (source_view_id.has_value()) {
        if (auto* window = window_of_view(*source_view_id); window && window->is_private() == is_private)
            return window;
    }
    for (auto* window : visible_browser_windows_active_first(m_active_window)) {
        if (window->is_private() == is_private)
            return window;
    }
    return nullptr;
}

void Application::open_view_in_new_tab(u64 view_id, Optional<u64> source_view_id, WebView::IsPrivate is_private, Web::HTML::ActivateTab activate_tab)
{
    if (auto* window = window_for_new_tab(source_view_id, is_private))
        window->create_new_tab(activate_tab, view_id);
    else
        new_window({}, configuration_for_new_window(), BrowserWindow::IsPopupWindow::No, is_private, view_id);
}

void Application::open_url_in_new_tab(URL::URL const& url, Web::HTML::ActivateTab activate_tab, Optional<u64> source_view_id, WebView::IsPrivate is_private) const
{
    auto* window = window_for_new_tab(source_view_id, is_private);
    if (!window) {
        const_cast<Application&>(*this).new_window({ url }, {}, BrowserWindow::IsPopupWindow::No, is_private);
        return;
    }

    window->new_tab_from_url(url, activate_tab, BrowserWindow::TabLocation::after_current_tab());
}

void Application::open_urls_in_new_tabs(ReadonlySpan<URL::URL> urls) const
{
    if (urls.is_empty())
        return;

    if (!m_active_window) {
        Vector<URL::URL> initial_urls;
        initial_urls.ensure_capacity(urls.size());
        for (auto const& url : urls)
            initial_urls.append(url);

        const_cast<Application&>(*this).new_window(initial_urls);
        return;
    }

    auto& window = active_window();
    auto* previous_tab = window.current_tab();
    for (auto const& url : urls) {
        auto location = previous_tab
            ? BrowserWindow::TabLocation::after_tab(*previous_tab)
            : BrowserWindow::TabLocation::end();
        auto& tab = window.new_tab_from_url(url, Web::HTML::ActivateTab::No, location);
        previous_tab = &tab;
    }
}

void Application::open_url_in_new_window(URL::URL const& url, WebView::IsPrivate is_private)
{
    new_window({ url }, configuration_for_new_window(), BrowserWindow::IsPopupWindow::No, is_private);
}

void Application::open_navigation_in_new_tab(Web::HTML::PreparedNavigationDescriptor navigation, Web::HTML::ActivateTab activate_tab, Optional<u64> source_view_id, WebView::IsPrivate is_private) const
{
    auto* window = window_for_new_tab(source_view_id, is_private);
    if (!window) {
        auto url = navigation.url;
        const_cast<Application&>(*this).new_window({ move(url) }, {}, BrowserWindow::IsPopupWindow::No, is_private, {}, ShowWindow::Yes, move(navigation));
        return;
    }

    auto& tab = window->create_new_tab(activate_tab, BrowserWindow::TabLocation::after_current_tab());
    tab.view().load(move(navigation));
}

void Application::open_navigation_in_new_window(Web::HTML::PreparedNavigationDescriptor navigation, WebView::IsPrivate is_private)
{
    auto url = navigation.url;
    new_window({ move(url) }, configuration_for_new_window(), BrowserWindow::IsPopupWindow::No, is_private, {}, ShowWindow::Yes, move(navigation));
}

void Application::ask_user_for_download_path(u64 request_id, ByteString const& file) const
{
    auto* dialog = new QFileDialog(nullptr, "Select save location", QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setAcceptMode(QFileDialog::AcceptSave);
    dialog->selectFile(qstring_from_ak_string(file.view()));

    QObject::connect(dialog, &QDialog::finished, [request_id, dialog = QPointer { dialog }](auto result) {
        Optional<ByteString> path;
        if (result == QDialog::Accepted && dialog && !dialog->selectedFiles().isEmpty())
            path = ak_byte_string_from_qstring(dialog->selectedFiles().first());
        if (auto* hub = HubConnection::the_if_connected())
            hub->async_answer_download_path(request_id, move(path));
    });

    dialog->open();
}

static QMessageBox* create_message_box(QMessageBox::Icon icon, StringView text, QWidget* parent)
{
    auto* dialog = new QMessageBox(parent);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle("Ladybird");
    dialog->setIcon(icon);
    dialog->setTextFormat(Qt::PlainText);
    dialog->setText(qstring_from_ak_string(text));
    dialog->addButton(QMessageBox::Ok);
    return dialog;
}

void Application::display_download_confirmation_dialog(StringView download_name, ByteString const& path) const
{
    auto message = MUST(String::formatted("{} saved to: {}", download_name, path));

    auto* dialog = create_message_box(QMessageBox::Information, message, active_window_if_any());

    auto* open_folder_button = dialog->addButton(QMessageBox::Open);
    open_folder_button->setText("Open folder");
    QObject::connect(open_folder_button, &QAbstractButton::clicked, [path_url = QUrl::fromLocalFile(qstring_from_ak_string(LexicalPath { path }.dirname()))] {
        QDesktopServices::openUrl(path_url);
    });

    dialog->open();
}

void Application::display_error_dialog(StringView error_message) const
{
    create_message_box(QMessageBox::Warning, error_message, active_window_if_any())->open();
}

void Application::open_file(String const& path) const
{
    QDesktopServices::openUrl(QUrl::fromLocalFile(qstring_from_ak_string(path)));
}

void Application::resolve_external_url_handler(URL::URL const& url, WebView::ExternalURLHandlerCallback callback) const
{
#if defined(AK_OS_LINUX)
    Ladybird::resolve_external_url_handler(url, ak_string_from_qstring(QGuiApplication::desktopFileName()), move(callback));
#elif defined(AK_OS_MACOS)
    Ladybird::resolve_external_url_handler(url, move(callback));
#else
    (void)url;
    callback(nullptr);
#endif
}

static QClipboard::Mode clipboard_mode(QClipboard const& clipboard, Application::ClipboardType type)
{
    switch (type) {
    case WebView::Application::ClipboardType::Text:
        return QClipboard::Clipboard;
    case WebView::Application::ClipboardType::Selection:
        return clipboard.supportsSelection() ? QClipboard::Selection : QClipboard::Clipboard;
    }
    VERIFY_NOT_REACHED();
}

bool Application::supports_clipboard_type(ClipboardType type) const
{
    switch (type) {
    case WebView::Application::ClipboardType::Text:
        return true;
    case WebView::Application::ClipboardType::Selection:
        return QGuiApplication::clipboard()->supportsSelection();
    }
    VERIFY_NOT_REACHED();
}

Utf16String Application::clipboard_text(ClipboardType type) const
{
    auto const* clipboard = QGuiApplication::clipboard();
    auto mode = clipboard_mode(*clipboard, type);

    return utf16_string_from_qstring(clipboard->text(mode));
}

void Application::set_clipboard_text(String text, ClipboardType type)
{
    auto* clipboard = QGuiApplication::clipboard();
    auto mode = clipboard_mode(*clipboard, type);

    clipboard->setText(qstring_from_ak_string(text), mode);
}

Web::Clipboard::SystemClipboardItem Application::clipboard_item() const
{
    Vector<Web::Clipboard::SystemClipboardRepresentation> representations;
    auto const* clipboard = QGuiApplication::clipboard();

    auto const* mime_data = clipboard->mimeData();
    if (!mime_data)
        return {};

    if (mime_data->hasUrls()) {
        QMimeDatabase mime_database;

        for (auto const& url : mime_data->urls()) {
            if (!url.isLocalFile())
                continue;

            auto file_path = url.toLocalFile();

            if (auto file = WebView::create_selected_file(ak_byte_string_from_qstring(file_path)); file.is_error()) {
                warnln("Unable to open clipboard file {}: {}", file_path, file.error());
            } else {
                auto mime_type = mime_database.mimeTypeForFile(file_path);
                representations.empend(ak_string_from_qstring(mime_type.name()), file.release_value());
            }
        }
    }

    if (representations.is_empty()) {
        for (auto const& format : mime_data->formats()) {
            auto mime_type = ak_string_from_qstring(format);
            if (mime_type == "text/uri-list"sv)
                continue;

            auto data = ak_byte_string_from_qbytearray(mime_data->data(format));
            representations.empend(move(mime_type), move(data));
        }
    }

    return { move(representations) };
}

void Application::insert_clipboard_item(Web::Clipboard::SystemClipboardItem item)
{
    auto* mime_data = new QMimeData();
    for (auto const& entry : item.system_clipboard_representations) {
        if (auto const* data = entry.data.get_pointer<ByteString>())
            mime_data->setData(qstring_from_ak_string(entry.name), qbytearray_from_ak_string(*data));
    }

    auto* clipboard = QGuiApplication::clipboard();
    clipboard->setMimeData(mime_data);
}

bool Application::supports_system_menu_bar() const
{
    return show_menu_bar_option_available();
}

void Application::update_tabs_display() const
{
    for (auto* widget : QApplication::topLevelWidgets()) {
        if (auto* window = as_if<BrowserWindow>(widget))
            window->update_tabs_display();
    }
}

void Application::rebuild_bookmarks_menu() const
{
    if (m_application)
        static_cast<LadybirdQApplication*>(m_application.ptr())->rebuild_bookmarks_menu();

    for (auto* widget : QApplication::topLevelWidgets()) {
        if (auto* window = as_if<BrowserWindow>(widget))
            window->rebuild_bookmarks_menu();
    }
}

void Application::update_reopen_recently_closed_actions() const
{
    if (m_application)
        static_cast<LadybirdQApplication*>(m_application.ptr())->update_reopen_recently_closed_action();
}

void Application::show_bookmark_context_menu(Gfx::IntPoint content_position, Optional<BookmarkItem const&> item, Optional<String const&> target_folder_id, Optional<String const&> parent_folder_id)
{
    if (auto* active_tab = this->active_tab(); active_tab && m_active_window) {
        auto position = active_tab->view().mapToGlobal(QPoint { content_position.x(), content_position.y() });
        m_active_window->bookmarks_bar().show_context_menu(position, item, target_folder_id, parent_folder_id);
    }
}

static NonnullRefPtr<Application::BookmarkPromise> display_add_or_edit_bookmark_dialog(
    QWidget* parent,
    BookmarkDialog::Type dialog_type,
    Optional<URL::URL const&> current_url,
    Optional<String const&> current_title,
    Optional<String> current_favicon_hash,
    Optional<String const&> selected_folder_id = {})
{
    auto const& folders = Application::bookmark_store().root_items();

    auto* dialog = new BookmarkDialog(parent, dialog_type, current_url, current_title, selected_folder_id, folders);
    auto promise = Application::BookmarkPromise::construct();

    QObject::connect(dialog, &QDialog::finished, [promise, current_favicon_hash = move(current_favicon_hash), dialog = QPointer { dialog }](auto result) mutable {
        if (result != QDialog::Accepted || !dialog) {
            promise->reject(Error::from_errno(ECANCELED));
            return;
        }

        auto url = WebView::sanitize_url(ak_string_from_qstring(dialog->url()));
        if (!url.has_value()) {
            promise->reject(Error::from_errno(EINVAL));
            return;
        }

        Optional<String> title;
        if (auto title_text = ak_string_from_qstring(dialog->title()); !title_text.is_empty())
            title = move(title_text);

        Optional<String> target_folder_id;
        if (auto target_folder_id_text = ak_string_from_qstring(dialog->selected_folder_id()); !target_folder_id_text.is_empty())
            target_folder_id = move(target_folder_id_text);

        promise->resolve(Application::BookmarkDialogResult {
            .data = BookmarkItem::Bookmark {
                .url = url.release_value(),
                .title = move(title),
                .favicon_hash = move(current_favicon_hash),
            },
            .target_folder_id = move(target_folder_id),
        });
    });

    dialog->open();
    return promise;
}

NonnullRefPtr<Application::BookmarkPromise> Application::display_add_bookmark_dialog(Optional<String const&> target_folder_id) const
{
    Optional<URL::URL> current_url;
    Optional<String> current_title;
    Optional<String> current_favicon_hash;

    if (auto* tab = active_tab(); tab && tab->is_loaded()) {
        auto const& view = tab->view();
        current_url = view.displayed_url();
        current_title = view.title().to_utf8();
        current_favicon_hash = view.favicon_hash();
    }

    return display_add_or_edit_bookmark_dialog(active_tab(), BookmarkDialog::Type::AddBookmark, current_url, current_title, current_favicon_hash, target_folder_id);
}

NonnullRefPtr<Application::BookmarkPromise> Application::display_edit_bookmark_dialog(BookmarkItem const& current_bookmark, Optional<String const&> parent_folder_id) const
{
    return display_add_or_edit_bookmark_dialog(active_tab(), BookmarkDialog::Type::EditBookmark, current_bookmark.bookmark().url, current_bookmark.bookmark().title, current_bookmark.bookmark().favicon_hash, parent_folder_id);
}

static NonnullRefPtr<Application::BookmarkPromise> display_add_or_edit_bookmark_folder_dialog(
    QWidget* parent,
    BookmarkDialog::Type dialog_type,
    Optional<String const&> current_title,
    Optional<String const&> selected_folder_id,
    Optional<String const&> excluded_folder_id = {})
{
    auto* dialog = new BookmarkDialog(parent, dialog_type, {}, current_title, selected_folder_id, Application::bookmark_store().root_items(), excluded_folder_id);
    auto promise = Application::BookmarkPromise::construct();

    QObject::connect(dialog, &QDialog::finished, [promise, dialog = QPointer { dialog }](auto result) {
        if (result != QDialog::Accepted || !dialog) {
            promise->reject(Error::from_errno(ECANCELED));
            return;
        }

        Optional<String> title;
        if (auto title_text = ak_string_from_qstring(dialog->title()); !title_text.is_empty())
            title = move(title_text);

        Optional<String> target_folder_id;
        if (auto target_folder_id_text = ak_string_from_qstring(dialog->selected_folder_id()); !target_folder_id_text.is_empty())
            target_folder_id = move(target_folder_id_text);

        promise->resolve(Application::BookmarkDialogResult {
            .data = BookmarkItem::Folder {
                .title = move(title),
                .children = {},
            },
            .target_folder_id = move(target_folder_id),
        });
    });

    dialog->open();
    return promise;
}

NonnullRefPtr<Application::BookmarkPromise> Application::display_add_bookmark_folder_dialog(Optional<String const&> default_title, Optional<String const&> target_folder_id) const
{
    return display_add_or_edit_bookmark_folder_dialog(active_tab(), BookmarkDialog::Type::AddFolder, default_title, target_folder_id);
}

NonnullRefPtr<Application::BookmarkPromise> Application::display_edit_bookmark_folder_dialog(BookmarkItem const& current_folder, Optional<String const&> parent_folder_id) const
{
    return display_add_or_edit_bookmark_folder_dialog(active_tab(), BookmarkDialog::Type::EditFolder, current_folder.folder().title, parent_folder_id, current_folder.id);
}

WebView::NotificationBackend& Application::notification_backend()
{
    if (m_notification_backend)
        return *m_notification_backend;

    m_notification_backend = create_notification_backend();
    if (!m_notification_backend)
        m_notification_backend = make<WebView::NoOpNotificationBackend>();
    m_notification_backend->on_shown = [](u64 id) {
        if (auto* hub = HubConnection::the_if_connected())
            hub->async_notification_shown(id);
    };
    m_notification_backend->on_failed = [](u64 id) {
        if (auto* hub = HubConnection::the_if_connected())
            hub->async_notification_failed(id);
    };
    m_notification_backend->on_clicked = [](u64 id) {
        if (auto* hub = HubConnection::the_if_connected())
            hub->async_notification_clicked(id);
    };
    m_notification_backend->on_closed = [](u64 id, WebView::NotificationBackend::ByUser by_user) {
        if (auto* hub = HubConnection::the_if_connected())
            hub->async_notification_closed(id, by_user == WebView::NotificationBackend::ByUser::Yes);
    };
    return *m_notification_backend;
}

void Application::resolve_external_url_handler_for_hub(u64 request_id, URL::URL const& url)
{
    resolve_external_url_handler(url, [this, request_id](RefPtr<WebView::ExternalURLHandler> handler) {
        auto* hub = HubConnection::the_if_connected();
        if (!hub)
            return;
        if (!handler) {
            hub->async_external_url_handler_resolved(request_id, Optional<u64> {}, String {}, false);
            return;
        }
        auto handler_id = m_next_external_url_handler_id++;
        auto application_name = handler->application_name();
        auto is_ladybird = handler->is_ladybird();
        m_external_url_handlers.set(handler_id, handler.release_nonnull());
        hub->async_external_url_handler_resolved(request_id, handler_id, move(application_name), is_ladybird);
    });
}

void Application::launch_external_url(u64 handler_id, u64 request_id, URL::URL const& url)
{
    auto handler = m_external_url_handlers.get(handler_id);
    if (!handler.has_value()) {
        HubConnection::the().async_external_url_launched(request_id, false);
        return;
    }
    (*handler)->launch(url, [request_id](bool launched) {
        if (auto* hub = HubConnection::the_if_connected())
            hub->async_external_url_launched(request_id, launched);
    });
}

void Application::release_external_url_handler(u64 handler_id)
{
    m_external_url_handlers.remove(handler_id);
}

void Application::report_active_view() const
{
    if (m_bookmarks)
        m_bookmarks->update_toggle_bookmark_action();
    if (m_application_actions)
        m_application_actions->update_for_active_view();

    auto* hub = HubConnection::the_if_connected();
    if (!hub)
        return;
    auto* tab = active_tab();
    hub->async_active_view_changed(tab && tab->is_loaded() ? Optional<u64> { tab->view().view_id() } : Optional<u64> {});
}

void Application::did_enable_devtools() const
{
    m_application_actions->devtools_enabled_changed(true);
    if (m_active_window)
        m_active_window->on_devtools_enabled();
}

void Application::did_disable_devtools() const
{
    m_application_actions->devtools_enabled_changed(false);
    if (m_active_window)
        m_active_window->on_devtools_disabled();
}

void Application::on_recently_closed_entries_changed() const
{
    update_reopen_recently_closed_actions();
}

QMenuBar* Application::create_application_menu_bar(ForBrowserWindow for_browser_window)
{
    auto& application = ApplicationActions::the();
    auto* menu_bar = new QMenuBar();

    auto* file_menu = menu_bar->addMenu("&File");
    file_menu->addAction(new_tab_action());
    file_menu->addAction(new_window_action());
    file_menu->addAction(new_private_window_action());
    file_menu->addAction(reopen_recently_closed_tab_action());
    if (for_browser_window == ForBrowserWindow::Yes)
        file_menu->addAction(close_current_tab_action());
    file_menu->addSeparator();
    file_menu->addAction(open_file_action());
    if (for_browser_window == ForBrowserWindow::Yes)
        file_menu->addAction(open_location_action());
    file_menu->addAction(open_downloads_action());
    file_menu->addSeparator();
    file_menu->addAction(quit_action());

    auto* edit_menu = menu_bar->addMenu("&Edit");
    if (for_browser_window == ForBrowserWindow::Yes) {
        edit_menu->addAction(create_application_action(*menu_bar, application.undo_action(), IncludeActionIcon::No));
        edit_menu->addAction(create_application_action(*menu_bar, application.redo_action(), IncludeActionIcon::No));
        edit_menu->addSeparator();
    }
    edit_menu->addAction(create_application_action(*menu_bar, application.cut_selection_action(), IncludeActionIcon::No));
    edit_menu->addAction(create_application_action(*menu_bar, application.copy_selection_action(), IncludeActionIcon::No));
    edit_menu->addAction(create_application_action(*menu_bar, application.paste_action(), IncludeActionIcon::No));
    edit_menu->addAction(create_application_action(*menu_bar, application.paste_and_match_style_action(), IncludeActionIcon::No));
    edit_menu->addAction(create_application_action(*menu_bar, application.select_all_action(), IncludeActionIcon::No));
    edit_menu->addSeparator();
    if (for_browser_window == ForBrowserWindow::Yes) {
        edit_menu->addAction(find_in_page_action());
        edit_menu->addAction(find_next_action());
        edit_menu->addAction(find_previous_action());
        edit_menu->addSeparator();
    }
    edit_menu->addAction(open_settings_action());

    auto* view_menu = menu_bar->addMenu("&View");
    if (for_browser_window == ForBrowserWindow::Yes) {
        view_menu->addAction(open_next_tab_action());
        view_menu->addAction(open_previous_tab_action());
        view_menu->addAction(search_tabs_action());
        view_menu->addSeparator();

        view_menu->addAction(toggle_sidebar_action());
        view_menu->addSeparator();

        view_menu->addAction(reload_action());
        view_menu->addAction(reload_bypassing_cache_action());
        view_menu->addAction(stop_action());
        // The menu stops the load of the tab it was opened over, so it's only enabled while that tab is loading.
        QObject::connect(view_menu, &QMenu::aboutToShow, view_menu, [] {
            auto* tab = Application::the().active_tab();
            Application::the().stop_action()->setEnabled(tab && tab->view().is_loading());
        });
        view_menu->addSeparator();

        view_menu->addAction(move_focus_action());
        view_menu->addSeparator();

        auto* zoom_menu = view_menu->addMenu("&Zoom");
        zoom_menu->addAction(zoom_in_action());
        zoom_menu->addAction(zoom_out_action());
        zoom_menu->addAction(reset_zoom_action());

        // The translation menu is the active tab's, so it's filled in as it opens.
        if (Application::the().has_translator()) {
            auto* translation_menu = view_menu->addMenu("&Translation");
            QObject::connect(translation_menu, &QMenu::aboutToShow, translation_menu, [translation_menu] {
                if (auto* tab = Application::the().active_tab())
                    repopulate_application_menu(*translation_menu, tab->view().translation_menu());
                else
                    translation_menu->clear();
            });
        }
        view_menu->addSeparator();
    }
    view_menu->addAction(create_application_action(*view_menu, application.force_dark_action(), IncludeActionIcon::No));
    view_menu->addMenu(create_application_menu(*view_menu, application.color_scheme_menu()));
    view_menu->addMenu(create_application_menu(*view_menu, application.contrast_menu()));
    view_menu->addMenu(create_application_menu(*view_menu, application.motion_menu()));
    view_menu->addSeparator();

    if constexpr (show_menu_bar_option_available())
        view_menu->addAction(create_application_action(*view_menu, application.toggle_menu_bar_action(), IncludeActionIcon::No));

    menu_bar->addMenu(bookmarks_menu());
    menu_bar->addMenu(history_menu());
    if (for_browser_window == ForBrowserWindow::Yes) {
        menu_bar->addMenu(inspect_menu());
        menu_bar->addMenu(debug_menu());
    } else {
        // NB: The Task Manager needs no browser window, so it can be opened while none is focused, such as right after
        //     it was closed.
        auto* inspect_menu = menu_bar->addMenu("&Inspect");
        inspect_menu->addAction(create_application_action(*inspect_menu, *m_open_task_manager_action, IncludeActionIcon::No));
    }
    menu_bar->addMenu(help_menu());

    return menu_bar;
}

#define DEFINE_APP_MENU_GETTER(type, name)                                      \
    type* Application::name()                                                   \
    {                                                                           \
        if (m_application)                                                      \
            return static_cast<LadybirdQApplication&>(*m_application).m_##name; \
        return nullptr;                                                         \
    }

DEFINE_APP_MENU_GETTER(QMenu, bookmarks_menu);
DEFINE_APP_MENU_GETTER(QMenu, history_menu);
DEFINE_APP_MENU_GETTER(QMenu, inspect_menu);
DEFINE_APP_MENU_GETTER(QMenu, debug_menu);
DEFINE_APP_MENU_GETTER(QMenu, help_menu);
DEFINE_APP_MENU_GETTER(QAction, new_tab_action);
DEFINE_APP_MENU_GETTER(QAction, new_window_action);
DEFINE_APP_MENU_GETTER(QAction, new_private_window_action);
DEFINE_APP_MENU_GETTER(QAction, reopen_recently_closed_tab_action);
DEFINE_APP_MENU_GETTER(QAction, close_current_tab_action);
DEFINE_APP_MENU_GETTER(QAction, open_next_tab_action);
DEFINE_APP_MENU_GETTER(QAction, open_previous_tab_action);
DEFINE_APP_MENU_GETTER(QAction, search_tabs_action);
DEFINE_APP_MENU_GETTER(QAction, move_focus_action);
DEFINE_APP_MENU_GETTER(QAction, open_file_action);
DEFINE_APP_MENU_GETTER(QAction, open_settings_action);
DEFINE_APP_MENU_GETTER(QAction, open_downloads_action);
DEFINE_APP_MENU_GETTER(QAction, toggle_bookmark_bar_action);
DEFINE_APP_MENU_GETTER(QAction, toggle_sidebar_action);
DEFINE_APP_MENU_GETTER(QAction, open_location_action);
DEFINE_APP_MENU_GETTER(QAction, reload_action);
DEFINE_APP_MENU_GETTER(QAction, reload_bypassing_cache_action);
DEFINE_APP_MENU_GETTER(QAction, stop_action);
DEFINE_APP_MENU_GETTER(QAction, find_next_action);
DEFINE_APP_MENU_GETTER(QAction, find_previous_action);
DEFINE_APP_MENU_GETTER(QAction, find_in_page_action);
DEFINE_APP_MENU_GETTER(QAction, zoom_in_action);
DEFINE_APP_MENU_GETTER(QAction, zoom_out_action);
DEFINE_APP_MENU_GETTER(QAction, reset_zoom_action);
DEFINE_APP_MENU_GETTER(QAction, quit_action);

}
