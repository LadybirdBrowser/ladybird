/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/ArgsParser.h>
#include <LibURL/InternalURLs.h>
#include <LibURL/URL.h>
#include <LibWebView/SessionStore.h>
#include <LibWebView/URL.h>
#include <LibWebView/Utilities.h>
#include <UI/Qt/Application.h>
#include <UI/Qt/BookmarkDialog.h>
#include <UI/Qt/ChromeLayout.h>
#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/EventLoopImplementationQt.h>
#include <UI/Qt/ExternalURLHandler.h>
#include <UI/Qt/Menu.h>
#include <UI/Qt/PictureInPictureWindow.h>
#include <UI/Qt/ProcessManagerWindow.h>
#include <UI/Qt/StringUtils.h>
#include <UI/Qt/WebContentView.h>

#if defined(AK_OS_MACOS)
#    include <UI/Qt/MacWindow.h>
#    include <UI/Qt/ScrollbarPreferencesMac.h>
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

#if defined(AK_OS_WINDOWS)
#    include <AK/Windows.h>
#    include <LibCore/TimeZoneWatcher.h>
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
        update_chrome_style();

        // Suppress some warnings that end up flooding the console when Orca is in use. QSpiAccessibleBridge logs both
        // (1) one-time at startup, a couple warnings for some AT-SPI2 methods it doesn't implement, and (2) for *every*
        // single Orca navigation action a user makes *every* time, warns about upstream Qt gaps in scroll-related stuff
        // we can't fix from here. So, this silences all those warnings. But if/when you *do* actually want to see the
        // messages — to debug Qt's bridge behavior, or whatever — you can override this with QT_LOGGING_RULES.
        QLoggingCategory::setFilterRules(QStringLiteral("qt.accessibility.atspi=false"));
    }

    virtual bool event(QEvent* event) override
    {
        auto& application = static_cast<Application&>(WebView::Application::the());
        auto const event_type = event->type();

#if defined(AK_OS_WINDOWS)
        static Optional<NativeWindowsTimeChangeEventFilter> time_change_event_filter {};
        if (auto time_zone_watcher = application.time_zone_watcher(); !time_change_event_filter.has_value() && time_zone_watcher.has_value()) {
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
        if (event_type == QEvent::ApplicationPaletteChange || event_type == QEvent::ThemeChange)
            update_chrome_style();

#if defined(AK_OS_MACOS)
        if (event_type == QEvent::ApplicationActivate && !has_visible_browser_window())
            application.open_new_window(WebView::IsPrivate::No);
#endif

        return handled;
    }

    void create_application_actions()
    {
        auto& application = WebView::Application::the();

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

        m_reopen_recently_closed_tab_action = make_action("&Reopen Recently Closed Tab");
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
#endif
        });
        QObject::connect(m_open_previous_tab_action, &QAction::triggered, this, []() {
            if (auto* window = Application::the().active_window_if_any())
                window->open_previous_tab();
        });

        m_open_file_action = make_action("&Open File...");
        m_open_file_action->setShortcut(QKeySequence(QKeySequence::StandardKey::Open));
        QObject::connect(m_open_file_action, &QAction::triggered, this, []() { Application::the().open_file(); });

        m_open_settings_action = create_application_action(*m_application_widget, application.open_settings_page_action(), IncludeActionIcon::No);
        m_open_downloads_action = create_application_action(*m_application_widget, application.open_downloads_page_action(), IncludeActionIcon::No);

        m_toggle_bookmark_bar_action = create_application_action(*m_application_widget, application.toggle_bookmark_bar_action(), IncludeActionIcon::No);

        m_find_in_page_action = make_action("&Find in Page...");
        m_find_in_page_action->setShortcuts(QKeySequence::keyBindings(QKeySequence::StandardKey::Find));
        QObject::connect(m_find_in_page_action, &QAction::triggered, this, []() {
            if (auto* window = Application::the().active_window_if_any())
                window->show_find_in_page();
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

        m_bookmarks_menu = create_application_menu(*m_application_widget, application.bookmarks_menu());

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

        m_reopen_recently_closed_tab_action->setText("&Reopen Recently Closed Tab");
        auto* active_window = Application::the().active_window_if_any();
        auto& session = active_window ? active_window->session() : WebView::Application::default_session();
        m_reopen_recently_closed_tab_action->setEnabled(session.session_store->has_closed_units());
    }

    void rebuild_bookmarks_menu()
    {
        if (m_bookmarks_menu)
            repopulate_application_menu(*m_bookmarks_menu, *m_application_widget, WebView::Application::the().bookmarks_menu());
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

    void update_chrome_style()
    {
        setStyleSheet(ChromeStyle::application_style_sheet(palette()));
    }

    // The application actions require a QWidget owner, which we don't have at the time of creation. We use this dummy
    // widget instead, which we also can't properly parent as it would need a QWidget owner as well.
    OwnPtr<QWidget> m_application_widget { nullptr };

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
    QAction* m_open_file_action { nullptr };
    QAction* m_open_settings_action { nullptr };
    QAction* m_open_downloads_action { nullptr };
    QAction* m_toggle_bookmark_bar_action { nullptr };
    QAction* m_find_in_page_action { nullptr };
    QAction* m_zoom_in_action { nullptr };
    QAction* m_zoom_out_action { nullptr };
    QAction* m_reset_zoom_action { nullptr };
    QAction* m_quit_action { nullptr };
};

Application::Application() = default;
Application::~Application() = default;

void Application::show_process_manager()
{
    if (!m_process_manager_window)
        m_process_manager_window = make<ProcessManagerWindow>(process_manager());
    m_process_manager_window->show();
    m_process_manager_window->raise();
    m_process_manager_window->activateWindow();
}

void Application::create_platform_actions()
{
    WebView::Application::inspect_menu().add_action(WebView::Action::create("Open Task Manager"sv, WebView::ActionID::OpenTaskManager, [this]() {
        show_process_manager();
    }));

    if (m_application) {
        auto& qapplication = static_cast<LadybirdQApplication&>(*m_application);
        qapplication.create_application_actions();
#if defined(AK_OS_MACOS)
        qapplication.create_application_menu_bar();
        qapplication.create_dock_menu();
#endif
    }
}

Core::EventLoop& Application::create_platform_event_loop()
{
    if (!browser_options().headless_mode.has_value()) {
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
        m_application = make<LadybirdQApplication>(arguments());
#if defined(AK_OS_LINUX)
        QGuiApplication::setDesktopFileName(QStringLiteral("org.ladybird.Ladybird"));
#endif
#if !defined(Q_OS_MACOS) && !defined(Q_OS_WIN)
        install_orca_scripts();
#endif
    }

    auto& event_loop = WebView::Application::create_platform_event_loop();

    if (!browser_options().headless_mode.has_value()) {
#if defined(AK_OS_LINUX)
        Ladybird::initialize_external_url_handler(event_loop);
#endif
#if defined(AK_OS_MACOS)
        Ladybird::on_scrollbar_track_press_behavior_change([this] {
            platform_scrollbar_track_press_behavior_changed();
        });
#endif
        static_cast<EventLoopImplementationQt&>(event_loop.impl()).set_main_loop();
    }

    return event_loop;
}

Optional<String> Application::ui_font_family() const
{
    if (!m_application)
        return {};
    return ak_string_from_qstring(QGuiApplication::font().family());
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

#if !defined(AK_OS_MACOS)
// On macOS, WebContent resolves system-ui with CoreText, so the system font family is not sent there.
Optional<String> Application::system_font_family() const
{
    return ui_font_family();
}
#endif

BrowserWindow& Application::new_window(Vector<URL::URL> const& initial_urls, WindowConfiguration const& configuration, BrowserWindow::IsPopupWindow is_popup_window, WebView::IsPrivate is_private, Tab* parent_tab, Optional<WebView::CanonicalTraversable&> traversable, ShowWindow show_window, Optional<Web::HTML::PreparedNavigationDescriptor> initial_navigation)
{
    auto* window = new BrowserWindow(initial_urls, is_popup_window, is_private, parent_tab, traversable);

    auto should_focus_location_editor = initial_urls.size() == 1 && initial_urls.first() == WebView::Application::settings().new_tab_page_url();
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

WebView::SessionLaunchPolicy Application::launch_restore_policy() const
{
    // Without the setting, only pinned tabs come back; with it, every window and tab does.
    if (WebView::Application::settings().restore_previous_session())
        return WebView::SessionLaunchPolicy::Everything;
    return WebView::SessionLaunchPolicy::PinnedOnly;
}

static WindowConfiguration window_configuration_for(WebView::SessionWindowGeometry const& geometry)
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
    auto plan = WebView::Application::default_session().session_store->take_launch_plan();

    BrowserWindow* frontmost_window = nullptr;
    for (size_t i = 0; i < plan.size(); ++i) {
        auto const& restored = plan[i];
        // The plan lists the frontmost window last, and opening the windows in that order leaves it on top.
        auto configuration = restored.geometry.has_value()
            ? window_configuration_for(*restored.geometry)
            : (i == plan.size() - 1 ? frontmost_window_configuration : configuration_for_new_window());

        auto* window = new BrowserWindow({}, BrowserWindow::IsPopupWindow::No, WebView::IsPrivate::No, nullptr, {}, restored.window_id);
        window->restore_tabs(restored.tabs, restored.active_tab_index);
        present_window(*window, configuration, ShowWindow::Yes);
        frontmost_window = window;
    }
    return frontmost_window;
}

void Application::open_startup_tabs(BrowserWindow& window, Vector<URL::URL> const& urls)
{
    auto const& new_tab_page_url = WebView::Application::settings().new_tab_page_url();
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

    auto const& window_settings = WebView::Application::settings().window_settings();
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
        new_window({ WebView::Application::settings().new_tab_page_url() });
        return;
    }

    m_active_window->open_new_tab();
}

void Application::open_new_window(WebView::IsPrivate is_private)
{
    // FIXME: Create a new tab page specific to private windows.
    new_window({ WebView::Application::settings().new_tab_page_url() }, configuration_for_new_window(), BrowserWindow::IsPopupWindow::No, is_private);
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

    WebView::Application::the().reset_private_browsing_session();
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
        new_window({ WebView::Application::settings().new_tab_page_url() });
        return;
    }

    if (auto* tab = m_active_window->current_tab())
        tab->focus_location_editor();
}

void Application::reopen_recently_closed_tab(BrowserWindow* target_window)
{
    auto is_private = target_window ? target_window->is_private() : WebView::IsPrivate::No;
    auto& session = target_window ? target_window->session() : WebView::Application::default_session();
    auto closed_unit = session.session_store->take_most_recently_closed();
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
    if (unit.was_window || !window) {
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

void Application::display_crash_report_notification(ByteString const& report_name)
{
    m_crash_report_notifier.offer(report_name);
}

void Application::review_crash_report(ByteString const& report_name)
{
    if (!m_active_window)
        return;

    // The tab is blank, so no web content runs behind the review, and it closes once the review is left.
    auto* current_tab = m_active_window->current_tab();
    auto& tab = m_active_window->new_tab_from_url(URL::about_blank(), Web::HTML::ActivateTab::Yes, BrowserWindow::TabLocation::after_current_tab());
    tab.view().review_hidden_crash_report(report_name);
    tab.enter_report_review(current_tab);
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
    WebView::Application::default_session().session_store->application_quitting();

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
    auto& downloader = file_downloader();

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

OwnPtr<WebView::PictureInPictureWindow> Application::create_picture_in_picture_window(WebView::WebContentPage& requesting_page, WebView::CanonicalTraversable& traversable, Gfx::IntSize video_size)
{
    if (browser_options().headless_mode.has_value())
        return WebView::Application::create_picture_in_picture_window(requesting_page, traversable, video_size);
    return Ladybird::create_picture_in_picture_window(traversable, requesting_page.view(), video_size);
}

Optional<WebView::ViewImplementation&> Application::active_web_view() const
{
    if (auto* active_tab = this->active_tab())
        return active_tab->view();
    return {};
}

Vector<WebView::BookmarkItem::Bookmark> Application::bookmarks_for_all_tabs_in_current_window() const
{
    if (!m_active_window)
        return {};

    // Every tab gets a bookmark, so this walks the tabs directly: for_each_tab() skips the ones that haven't loaded.
    Vector<WebView::BookmarkItem::Bookmark> bookmarks;
    for (int i = 0; i < m_active_window->tab_count(); ++i) {
        auto& tab = *m_active_window->tab_at(i);
        if (tab.is_loaded()) {
            auto const& view = tab.view();
            bookmarks.append(WebView::BookmarkItem::Bookmark {
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
        bookmarks.append(WebView::BookmarkItem::Bookmark {
            .url = url,
            .title = has_title ? ak_string_from_qstring(title) : Optional<String> {},
            .favicon_hash = {},
        });
    }
    return bookmarks;
}

Optional<WebView::ViewImplementation&> Application::activate_tab_with_url(URL::URL const& url, URL::ExcludeFragment exclude_fragment) const
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
        return tab->view();
    }
    return {};
}

Optional<WebView::ViewImplementation&> Application::open_blank_new_tab(Web::HTML::ActivateTab activate_tab) const
{
    if (!m_active_window) {
        auto& window = const_cast<Application&>(*this).new_window({ WebView::Application::settings().new_tab_page_url() });
        if (auto* tab = window.current_tab())
            return tab->view();
        return {};
    }

    auto& tab = active_window().create_new_tab(activate_tab, BrowserWindow::TabLocation::end());
    return tab.view();
}

void Application::open_url_in_new_tab(URL::URL const& url, Web::HTML::ActivateTab activate_tab) const
{
    if (!m_active_window) {
        const_cast<Application&>(*this).new_window({ url });
        return;
    }

    active_window().new_tab_from_url(url, activate_tab, BrowserWindow::TabLocation::after_current_tab());
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

void Application::open_navigation_in_new_tab(Web::HTML::PreparedNavigationDescriptor navigation, Web::HTML::ActivateTab activate_tab) const
{
    if (!m_active_window) {
        auto url = navigation.url;
        const_cast<Application&>(*this).new_window({ move(url) }, {}, BrowserWindow::IsPopupWindow::No, WebView::IsPrivate::No, nullptr, {}, ShowWindow::Yes, move(navigation));
        return;
    }

    auto& tab = active_window().create_new_tab(activate_tab, BrowserWindow::TabLocation::after_current_tab());
    tab.view().load(move(navigation));
}

void Application::open_navigation_in_new_window(Web::HTML::PreparedNavigationDescriptor navigation, WebView::IsPrivate is_private)
{
    auto url = navigation.url;
    new_window({ move(url) }, configuration_for_new_window(), BrowserWindow::IsPopupWindow::No, is_private, nullptr, {}, ShowWindow::Yes, move(navigation));
}

Optional<ByteString> Application::ask_user_for_download_path(ByteString const& file) const
{
    auto default_path = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);

    if (default_path.isNull() || default_path.isEmpty())
        default_path = qstring_from_ak_string(file.view());
    else
        default_path = QDir { default_path }.filePath(qstring_from_ak_string(file.view()));

    auto path = QFileDialog::getSaveFileName(nullptr, "Select save location", default_path);
    if (path.isNull())
        return {};

    return ak_byte_string_from_qstring(path);
}

void Application::display_download_confirmation_dialog(StringView download_name, LexicalPath const& path) const
{
    auto message = MUST(String::formatted("{} saved to: {}", download_name, path));

    QMessageBox dialog(active_tab());
    dialog.setWindowTitle("Ladybird");
    dialog.setIcon(QMessageBox::Information);
    dialog.setText(qstring_from_ak_string(message));
    dialog.addButton(QMessageBox::Ok);
    dialog.addButton(QMessageBox::Open)->setText("Open folder");

    if (dialog.exec() == QMessageBox::Open) {
        auto path_url = QUrl::fromLocalFile(qstring_from_ak_string(path.dirname()));
        QDesktopServices::openUrl(path_url);
    }
}

void Application::display_error_dialog(StringView error_message) const
{
    if (browser_options().headless_mode.has_value()) {
        WebView::Application::display_error_dialog(error_message);
        return;
    }

    QMessageBox::warning(active_tab(), "Ladybird", qstring_from_ak_string(error_message));
}

void Application::open_download(WebView::FileDownloader::Download const& download) const
{
    auto path = download_file_path_for_frontend_action(download);
    if (path.is_error()) {
        display_error_dialog("Unable to open downloaded file: path cannot be represented by this frontend"sv);
        return;
    }

    QDesktopServices::openUrl(QUrl::fromLocalFile(qstring_from_ak_string(path.release_value())));
}

void Application::show_download_in_folder(WebView::FileDownloader::Download const& download) const
{
    auto path = download_directory_path_for_frontend_action(download);
    if (path.is_error()) {
        display_error_dialog("Unable to show downloaded file: path cannot be represented by this frontend"sv);
        return;
    }

    QDesktopServices::openUrl(QUrl::fromLocalFile(qstring_from_ak_string(path.release_value())));
}

void Application::resolve_external_url_handler(URL::URL const& url, WebView::ExternalURLHandlerCallback callback) const
{
    if (browser_options().headless_mode.has_value()) {
        WebView::Application::resolve_external_url_handler(url, move(callback));
        return;
    }

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
    if (browser_options().headless_mode.has_value())
        return WebView::Application::supports_clipboard_type(type);

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
    if (browser_options().headless_mode.has_value())
        return WebView::Application::clipboard_text(type);

    auto const* clipboard = QGuiApplication::clipboard();
    auto mode = clipboard_mode(*clipboard, type);

    return utf16_string_from_qstring(clipboard->text(mode));
}

void Application::set_clipboard_text(String text, ClipboardType type)
{
    if (browser_options().headless_mode.has_value()) {
        WebView::Application::set_clipboard_text(text, type);
        return;
    }

    auto* clipboard = QGuiApplication::clipboard();
    auto mode = clipboard_mode(*clipboard, type);

    clipboard->setText(qstring_from_ak_string(text), mode);
}

Web::Clipboard::SystemClipboardItem Application::clipboard_item() const
{
    if (browser_options().headless_mode.has_value())
        return WebView::Application::clipboard_item();

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
    if (browser_options().headless_mode.has_value()) {
        WebView::Application::insert_clipboard_item(move(item));
        return;
    }

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

void Application::show_bookmark_context_menu(Gfx::IntPoint content_position, Optional<WebView::BookmarkItem const&> item, Optional<String const&> target_folder_id, Optional<String const&> parent_folder_id)
{
    if (auto* active_tab = this->active_tab()) {
        auto position = active_tab->view().mapToGlobal(QPoint { content_position.x(), content_position.y() });
        active_tab->bookmarks_bar().show_context_menu(position, item, target_folder_id, parent_folder_id);
    }
}

Optional<Application::BookmarkID> Application::bookmark_item_id_for_context_menu() const
{
    if (auto* active_tab = this->active_tab()) {
        auto const& bookmarks_bar = active_tab->bookmarks_bar();

        return Application::BookmarkID {
            .id = bookmarks_bar.selected_bookmark_menu_item_id(),
            .target_folder_id = bookmarks_bar.selected_bookmark_menu_target_folder_id(),
            .parent_folder_id = bookmarks_bar.selected_bookmark_menu_parent_folder_id(),
        };
    }

    return {};
}

static NonnullRefPtr<WebView::Application::BookmarkPromise> display_add_or_edit_bookmark_dialog(
    QWidget* parent,
    BookmarkDialog::Type dialog_type,
    Optional<URL::URL const&> current_url,
    Optional<String const&> current_title,
    Optional<String> current_favicon_hash,
    Optional<String const&> selected_folder_id = {})
{
    auto const& folders = WebView::Application::bookmark_store().root_items();

    auto* dialog = new BookmarkDialog(parent, dialog_type, current_url, current_title, selected_folder_id, folders);
    auto promise = WebView::Application::BookmarkPromise::construct();

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

        promise->resolve(WebView::Application::BookmarkDialogResult {
            .data = WebView::BookmarkItem::Bookmark {
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

    if (auto view = active_web_view(); view.has_value()) {
        current_url = view->displayed_url();
        current_title = view->title().to_utf8();
        current_favicon_hash = view->favicon_hash();
    }

    return display_add_or_edit_bookmark_dialog(active_tab(), BookmarkDialog::Type::AddBookmark, current_url, current_title, current_favicon_hash, target_folder_id);
}

NonnullRefPtr<Application::BookmarkPromise> Application::display_edit_bookmark_dialog(WebView::BookmarkItem const& current_bookmark, Optional<String const&> parent_folder_id) const
{
    return display_add_or_edit_bookmark_dialog(active_tab(), BookmarkDialog::Type::EditBookmark, current_bookmark.bookmark().url, current_bookmark.bookmark().title, current_bookmark.bookmark().favicon_hash, parent_folder_id);
}

static NonnullRefPtr<WebView::Application::BookmarkPromise> display_add_or_edit_bookmark_folder_dialog(
    QWidget* parent,
    BookmarkDialog::Type dialog_type,
    Optional<String const&> current_title,
    Optional<String const&> selected_folder_id,
    Optional<String const&> excluded_folder_id = {})
{
    auto* dialog = new BookmarkDialog(parent, dialog_type, {}, current_title, selected_folder_id, WebView::Application::bookmark_store().root_items(), excluded_folder_id);
    auto promise = WebView::Application::BookmarkPromise::construct();

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

        promise->resolve(WebView::Application::BookmarkDialogResult {
            .data = WebView::BookmarkItem::Folder {
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

NonnullRefPtr<Application::BookmarkPromise> Application::display_edit_bookmark_folder_dialog(WebView::BookmarkItem const& current_folder, Optional<String const&> parent_folder_id) const
{
    return display_add_or_edit_bookmark_folder_dialog(active_tab(), BookmarkDialog::Type::EditFolder, current_folder.folder().title, parent_folder_id, current_folder.id);
}

void Application::on_devtools_enabled() const
{
    WebView::Application::on_devtools_enabled();

    if (m_active_window)
        m_active_window->on_devtools_enabled();
}

void Application::on_devtools_disabled() const
{
    WebView::Application::on_devtools_disabled();

    if (m_active_window)
        m_active_window->on_devtools_disabled();
}

void Application::on_recently_closed_entries_changed() const
{
    update_reopen_recently_closed_actions();
}

QMenuBar* Application::create_application_menu_bar(ForBrowserWindow for_browser_window)
{
    auto& application = WebView::Application::the();
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
    edit_menu->addAction(create_application_action(*menu_bar, application.select_all_action(), IncludeActionIcon::No));
    edit_menu->addSeparator();
    if (for_browser_window == ForBrowserWindow::Yes) {
        edit_menu->addAction(find_in_page_action());
        edit_menu->addSeparator();
    }
    edit_menu->addAction(open_settings_action());

    auto* view_menu = menu_bar->addMenu("&View");
    if (for_browser_window == ForBrowserWindow::Yes) {
        view_menu->addAction(open_next_tab_action());
        view_menu->addAction(open_previous_tab_action());
        view_menu->addSeparator();

        auto* zoom_menu = view_menu->addMenu("&Zoom");
        zoom_menu->addAction(zoom_in_action());
        zoom_menu->addAction(zoom_out_action());
        zoom_menu->addAction(reset_zoom_action());
        view_menu->addSeparator();
    }
    view_menu->addMenu(create_application_menu(*view_menu, application.color_scheme_menu()));
    view_menu->addMenu(create_application_menu(*view_menu, application.contrast_menu()));
    view_menu->addMenu(create_application_menu(*view_menu, application.motion_menu()));

    if constexpr (show_menu_bar_option_available()) {
        view_menu->addSeparator();
        view_menu->addAction(create_application_action(*view_menu, application.toggle_menu_bar_action(), IncludeActionIcon::No));
    }

    menu_bar->addMenu(bookmarks_menu());
    menu_bar->addMenu(history_menu());
    if (for_browser_window == ForBrowserWindow::Yes) {
        menu_bar->addMenu(inspect_menu());
        menu_bar->addMenu(debug_menu());
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
DEFINE_APP_MENU_GETTER(QAction, open_file_action);
DEFINE_APP_MENU_GETTER(QAction, open_settings_action);
DEFINE_APP_MENU_GETTER(QAction, open_downloads_action);
DEFINE_APP_MENU_GETTER(QAction, toggle_bookmark_bar_action);
DEFINE_APP_MENU_GETTER(QAction, find_in_page_action);
DEFINE_APP_MENU_GETTER(QAction, zoom_in_action);
DEFINE_APP_MENU_GETTER(QAction, zoom_out_action);
DEFINE_APP_MENU_GETTER(QAction, reset_zoom_action);
DEFINE_APP_MENU_GETTER(QAction, quit_action);

}
