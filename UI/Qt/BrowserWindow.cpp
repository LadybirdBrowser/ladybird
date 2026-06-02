/*
 * Copyright (c) 2022-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Matthew Costa <ucosty@gmail.com>
 * Copyright (c) 2022, Filiph Sandström <filiph.sandstrom@filfatstudios.com>
 * Copyright (c) 2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024-2026, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2025, Simon Farre <simon.farre.cx@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/Platform.h>
#include <AK/RefPtr.h>
#include <AK/StdLibExtras.h>
#include <AK/TypeCasts.h>
#include <LibWebView/Application.h>
#include <LibWebView/HistoryStore.h>
#include <UI/Qt/Application.h>
#include <UI/Qt/BrowserWindow.h>
#include <UI/Qt/ChromeLayout.h>
#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/DevToolsBanner.h>
#include <UI/Qt/Icon.h>
#if defined(AK_OS_MACOS)
#    include <UI/Qt/MacWindow.h>
#endif
#include <UI/Qt/Menu.h>
#include <UI/Qt/Settings.h>
#include <UI/Qt/StringUtils.h>
#include <UI/Qt/TabBar.h>
#include <UI/Qt/WebContentView.h>
#include <UI/Qt/WindowControlButton.h>

#include <QAbstractButton>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCursor>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPlatformSurfaceEvent>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QScreen>
#include <QShortcut>
#include <QTabBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWidget>
#include <QWindow>

namespace Ladybird {

static constexpr auto AUDIO_STATE_BUTTON_POSITION = QTabBar::LeftSide;
static constexpr auto TAB_CLOSE_BUTTON_POSITION = QTabBar::RightSide;
static constexpr auto WINDOW_DRAG_REGION_PROPERTY = "LadybirdWindowDragRegion";
#if defined(AK_OS_MACOS)
static constexpr qreal WINDOW_CORNER_RADIUS = 12.0;
#endif

static bool should_use_screen_signal_for_dpi_changes()
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    return QGuiApplication::platformName() != "wayland";
#else
    return true;
#endif
}

static Optional<u64> display_id_for_screen(QScreen* screen)
{
    if (!screen)
        return {};

    // Qt does not expose a portable physical display identifier. The compositor only
    // needs a stable per-process grouping key for Qt-backed windows.
    static u64 next_display_id = 1;
    static HashMap<QScreen*, u64> display_ids;
    return display_ids.ensure(screen, [] {
        return next_display_id++;
    });
}

static QString reopen_recently_closed_action_text(Optional<WebView::RecentlyClosedEntry const&> entry)
{
    if (entry.has_value() && entry->was_window)
        return "&Reopen Recently Closed Window";

    return "&Reopen Recently Closed Tab";
}

static Vector<URL::URL> recently_closed_urls_for_window(TabWidget const& tabs_container)
{
    Vector<URL::URL> urls;
    urls.ensure_capacity(tabs_container.count());

    for (int index = 0; index < tabs_container.count(); ++index)
        urls.append(tabs_container.tab(index)->view().url());

    return urls;
}

FullscreenMode::FullscreenMode(BrowserWindow* window, ExitFullscreenButton* exit_button)
    : QObject(window)
    , m_window(window)
    , m_exit_button(exit_button)
{
    connect(m_exit_button, &QPushButton::clicked, this, [this]() {
        exit(ExitInitiatedBy::UI);
    });
}

void FullscreenMode::exit(ExitInitiatedBy initiated_by)
{
    if (is_api_fullscreen()) {
        qApp->removeEventFilter(this);
        if (initiated_by == ExitInitiatedBy::UI && m_window->tab_index(m_fullscreen_tab) != -1) {
            m_fullscreen_tab->view().exit_fullscreen();
        }
        emit on_exit_fullscreen();
    }
    m_fullscreen_tab = nullptr;
}

void FullscreenMode::enter(Tab* tab)
{
    qApp->installEventFilter(this);
    m_fullscreen_tab = tab;
    m_window->enter_fullscreen();
}

void FullscreenMode::entered_fullscreen()
{
    m_debounce = true;
    m_exit_button->animate_show();
    // Let button float in place 3 * time it takes to animate it in place
    QTimer::singleShot(button_animation_time() * 3, [this]() { m_debounce = false; });
}

bool FullscreenMode::is_api_fullscreen() const
{
    return m_fullscreen_tab;
}

bool FullscreenMode::debounce() const
{
    return m_debounce;
}

void FullscreenMode::maybe_animate_show_exit_button(QPointF pos)
{
    u64 const mouse_y = static_cast<u64>(pos.y());
    u64 const threshold = static_cast<u64>(m_window->height() * 0.01);

    if (debounce()) {
        return;
    }

    // Display the button if the mouse is 1% from the top
    if (mouse_y <= threshold) {
        if (!m_exit_button->isVisible()) {
            m_debounce = true;
            m_exit_button->animate_show();
            QTimer::singleShot(button_animation_time() * 3, [this]() { m_debounce = false; });
        }
    } else if (mouse_y > (threshold * 10) && m_exit_button->isVisible()) {
        // if the button has floated in, we want to hide it when leaving the top 10%
        m_exit_button->hide();
    }
}

bool FullscreenMode::eventFilter(QObject* obj, QEvent* event)
{
    ASSERT(is_api_fullscreen());
    if (event->type() == QEvent::MouseMove) {
        QMouseEvent* mouse_event = static_cast<QMouseEvent*>(event);
        maybe_animate_show_exit_button(mouse_event->pos());
    }

    return QObject::eventFilter(obj, event);
}

ExitFullscreenButton::ExitFullscreenButton(QWidget* parent)
    : QPushButton("Exit fullscreen", parent)
{
    setStyleSheet("background-color:rgb(55, 99, 129); color: white; padding: 10px; border-radius: 5px;");
    adjustSize();
    hide();
    m_widget_animation = new QPropertyAnimation(this, "pos");
}

void ExitFullscreenButton::animate_show()
{
    if (isVisible())
        return;

    show();
    QScreen* current_screen = screen();
    QRect screen_geometry = current_screen->geometry();

    int const destination_x = (screen_geometry.width() - width()) / 2;
    int const destination_y = static_cast<int>(static_cast<float>(screen_geometry.height()) * 0.05);

    m_widget_animation->setDuration(FullscreenMode::button_animation_time());
    m_widget_animation->setStartValue(QPoint(destination_x, -height()));
    m_widget_animation->setEndValue(QPoint(destination_x, destination_y));
    m_widget_animation->setEasingCurve(QEasingCurve::OutBounce);
    m_widget_animation->start();
}

static QIcon const& app_icon()
{
    static QIcon icon;
    if (icon.isNull()) {
        QPixmap pixmap;
        pixmap.load(":/Icons/ladybird.png");
        icon = QIcon(pixmap);
    }
    return icon;
}

BrowserWindow::BrowserWindow(Vector<URL::URL> const& initial_urls, IsPopupWindow is_popup_window, Tab* parent_tab, Optional<u64> page_index)
    : m_tabs_container(new TabWidget(this))
    , m_is_popup_window(is_popup_window)
{
    auto const& browser_options = WebView::Application::browser_options();

    setWindowFlag(Qt::FramelessWindowHint);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setWindowIcon(app_icon());
    qApp->installEventFilter(this);
    update_window_corners();

    update_tabs_display();

    // Listen for DPI changes
    m_device_pixel_ratio = devicePixelRatio();
    m_current_screen = screen();
    m_display_id = display_id_for_screen(m_current_screen);
    if (m_current_screen)
        m_refresh_rate = m_current_screen->refreshRate();

    if (should_use_screen_signal_for_dpi_changes()) {
        setAttribute(Qt::WA_NativeWindow);
        setAttribute(Qt::WA_DontCreateNativeAncestors);
    }
    connect_screen_signals(m_current_screen);
    connect_window_screen_changed_signal();

    m_hamburger_menu = new QMenu(this);

    menuBar()->setObjectName("LadybirdMenuBar");
    create_menu_bar_window_controls();
    update_menu_bar_style();
    update_menu_bar_visibility(Settings::the()->show_menubar());

    QObject::connect(Settings::the(), &Settings::show_menubar_changed, this, [this](bool show_menubar) {
        update_menu_bar_visibility(show_menubar);
    });

    auto* file_menu = menuBar()->addMenu("&File");

    m_new_tab_action = new QAction("New &Tab", this);
    m_new_tab_action->setShortcuts(QKeySequence::keyBindings(QKeySequence::StandardKey::AddTab));
    m_hamburger_menu->addAction(m_new_tab_action);
    file_menu->addAction(m_new_tab_action);

    m_new_window_action = new QAction("New &Window", this);
    m_new_window_action->setShortcuts(QKeySequence::keyBindings(QKeySequence::StandardKey::New));
    m_hamburger_menu->addAction(m_new_window_action);
    file_menu->addAction(m_new_window_action);

    m_reopen_recently_closed_tab_action = new QAction("&Reopen Recently Closed Tab", this);
    m_reopen_recently_closed_tab_action->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_T));
    m_hamburger_menu->addAction(m_reopen_recently_closed_tab_action);
    file_menu->addAction(m_reopen_recently_closed_tab_action);
    update_reopen_recently_closed_action();

    auto* close_current_tab_action = new QAction("&Close Current Tab", this);
    close_current_tab_action->setShortcuts(QKeySequence::keyBindings(QKeySequence::StandardKey::Close));
    m_hamburger_menu->addAction(close_current_tab_action);
    file_menu->addAction(close_current_tab_action);

    auto* open_file_action = new QAction("&Open File...", this);
    open_file_action->setShortcut(QKeySequence(QKeySequence::StandardKey::Open));
    m_hamburger_menu->addAction(open_file_action);
    file_menu->addAction(open_file_action);

    m_hamburger_menu->addSeparator();

    auto* edit_menu = m_hamburger_menu->addMenu("&Edit");
    menuBar()->addMenu(edit_menu);

    edit_menu->addAction(create_application_action(*this, Application::the().cut_selection_action(), IncludeActionIcon::No));
    edit_menu->addAction(create_application_action(*this, Application::the().copy_selection_action(), IncludeActionIcon::No));
    edit_menu->addAction(create_application_action(*this, Application::the().paste_action(), IncludeActionIcon::No));
    edit_menu->addAction(create_application_action(*this, Application::the().select_all_action(), IncludeActionIcon::No));
    edit_menu->addSeparator();

    m_find_in_page_action = new QAction("&Find in Page...", this);
    m_find_in_page_action->setShortcuts(QKeySequence::keyBindings(QKeySequence::StandardKey::Find));

    auto find_previous_shortcuts = QKeySequence::keyBindings(QKeySequence::StandardKey::FindPrevious);
    for (auto const& shortcut : find_previous_shortcuts)
        new QShortcut(shortcut, this, [this] {
            if (m_current_tab)
                m_current_tab->find_previous();
        });

    auto find_next_shortcuts = QKeySequence::keyBindings(QKeySequence::StandardKey::FindNext);
    for (auto const& shortcut : find_next_shortcuts)
        new QShortcut(shortcut, this, [this] {
            if (m_current_tab)
                m_current_tab->find_next();
        });

    edit_menu->addAction(m_find_in_page_action);
    QObject::connect(m_find_in_page_action, &QAction::triggered, this, &BrowserWindow::show_find_in_page);

    edit_menu->addSeparator();
    edit_menu->addAction(create_application_action(*edit_menu, Application::the().open_settings_page_action(), IncludeActionIcon::No));

    auto* view_menu = m_hamburger_menu->addMenu("&View");
    menuBar()->addMenu(view_menu);

    auto* open_next_tab_action = new QAction("Open &Next Tab", this);
    open_next_tab_action->setShortcuts({
        QKeySequence(Qt::CTRL | Qt::Key_PageDown),
        QKeySequence(Qt::CTRL | Qt::Key_Tab),
#if defined(AK_OS_MACOS)
        QKeySequence(Qt::META | Qt::Key_Tab),
        QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_BracketRight),
#endif
    });
    view_menu->addAction(open_next_tab_action);
    QObject::connect(open_next_tab_action, &QAction::triggered, this, &BrowserWindow::open_next_tab);

    auto* open_previous_tab_action = new QAction("Open &Previous Tab", this);
    open_previous_tab_action->setShortcuts({
        QKeySequence(Qt::CTRL | Qt::Key_PageUp),
        QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Tab),
#if defined(AK_OS_MACOS)
        QKeySequence(Qt::META | Qt::SHIFT | Qt::Key_Tab),
        QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_BracketLeft),
#endif
    });
    view_menu->addAction(open_previous_tab_action);
    QObject::connect(open_previous_tab_action, &QAction::triggered, this, &BrowserWindow::open_previous_tab);

    view_menu->addSeparator();

    view_menu->addMenu(create_application_menu(*view_menu, Application::the().zoom_menu()));
    view_menu->addSeparator();

    view_menu->addMenu(create_application_menu(*view_menu, Application::the().color_scheme_menu()));
    view_menu->addMenu(create_application_menu(*view_menu, Application::the().contrast_menu()));
    view_menu->addMenu(create_application_menu(*view_menu, Application::the().motion_menu()));
    view_menu->addSeparator();

    if (show_menubar_option_available()) {
        auto* show_menubar = new QAction("Show &Menubar", this);
        show_menubar->setCheckable(true);
        show_menubar->setChecked(Settings::the()->show_menubar());
        view_menu->addAction(show_menubar);
        QObject::connect(show_menubar, &QAction::triggered, this, [](bool checked) {
            Settings::the()->set_show_menubar(checked);
        });
    }

    m_bookmarks_menu = create_application_menu(*this, Application::the().bookmarks_menu());
    m_hamburger_menu->addMenu(m_bookmarks_menu);
    menuBar()->addMenu(m_bookmarks_menu);

    auto* inspect_menu = create_application_menu(*m_hamburger_menu, Application::the().inspect_menu());
    m_hamburger_menu->addMenu(inspect_menu);
    menuBar()->addMenu(inspect_menu);

    auto* debug_menu = create_application_menu(*m_hamburger_menu, Application::the().debug_menu());
    m_hamburger_menu->addMenu(debug_menu);
    menuBar()->addMenu(debug_menu);

    auto* help_menu = m_hamburger_menu->addMenu("&Help");
    menuBar()->addMenu(help_menu);

    help_menu->addAction(create_application_action(*help_menu, Application::the().open_about_page_action(), IncludeActionIcon::No));

    m_hamburger_menu->addSeparator();
    file_menu->addSeparator();

    auto* quit_action = new QAction("&Quit", this);
    quit_action->setShortcuts(QKeySequence::keyBindings(QKeySequence::StandardKey::Quit));
    m_hamburger_menu->addAction(quit_action);
    file_menu->addAction(quit_action);
    QObject::connect(quit_action, &QAction::triggered, this, &QMainWindow::close);

    QObject::connect(m_new_tab_action, &QAction::triggered, this, [this] {
        auto& tab = new_tab_from_url(WebView::Application::settings().new_tab_page_url(), Web::HTML::ActivateTab::Yes);
        tab.set_url_is_hidden(true);
        tab.focus_location_editor();
    });
    QObject::connect(m_new_window_action, &QAction::triggered, this, [] {
        auto const& previous_active_window = Application::the().active_window();
        WindowConfiguration configuration {
            .width = previous_active_window.width(),
            .height = previous_active_window.height(),
            .maximized = previous_active_window.isMaximized(),
        };
        Application::the().new_window({ WebView::Application::settings().new_tab_page_url() }, configuration);
    });
    QObject::connect(m_reopen_recently_closed_tab_action, &QAction::triggered, this, [this] {
        auto recently_closed_entry = Application::history_store().pop_most_recently_closed_entry();
        if (recently_closed_entry.has_value()) {
            if (recently_closed_entry->was_window) {
                auto& window = Application::the().new_window(recently_closed_entry->urls);
                window.activate_tab(static_cast<int>(recently_closed_entry->active_tab_index));
            } else if (!recently_closed_entry->urls.is_empty()) {
                new_tab_from_url(recently_closed_entry->urls[0], Web::HTML::ActivateTab::Yes);
            }
        }
        Application::the().update_reopen_recently_closed_actions();
    });
    QObject::connect(open_file_action, &QAction::triggered, this, &BrowserWindow::open_file);

    m_exit_button = new ExitFullscreenButton { this };
    m_fullscreen_mode = new FullscreenMode { this, m_exit_button };
    connect(m_fullscreen_mode, &FullscreenMode::on_exit_fullscreen, this, &BrowserWindow::exit_fullscreen);
    connect(m_fullscreen_mode, &FullscreenMode::on_exit_fullscreen, m_exit_button, &ExitFullscreenButton::hide);

    QObject::connect(m_tabs_container, &TabWidget::current_tab_changed, this, [this](int index) {
        auto* tab = m_tabs_container->tab(index);
        if (tab)
            setWindowTitle(QString("%1 - Ladybird").arg(tab->title()));

        set_current_tab(tab);
        if (tab) {
            if (auto* focus_widget = tab->focusWidget(); focus_widget && tab->isAncestorOf(focus_widget))
                focus_widget->setFocus();
            else
                tab->view().setFocus();
        }
        fullscreen_mode().exit(FullscreenMode::ExitInitiatedBy::UI);
    });
    QObject::connect(m_tabs_container, &TabWidget::tab_close_requested, this, &BrowserWindow::request_to_close_tab);
    QObject::connect(close_current_tab_action, &QAction::triggered, this, &BrowserWindow::request_to_close_current_tab);

    for (int i = 0; i <= 7; ++i) {
        new QShortcut(QKeySequence(Qt::CTRL | static_cast<Qt::Key>(Qt::Key_1 + i)), this, [this, i] {
            if (m_tabs_container->count() <= 1)
                return;

            m_tabs_container->set_current_index(min(i, m_tabs_container->count() - 1));
        });
    }

    new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_9), this, [this] {
        if (m_tabs_container->count() <= 1)
            return;

        m_tabs_container->set_current_index(m_tabs_container->count() - 1);
    });

    if (parent_tab) {
        new_child_tab(Web::HTML::ActivateTab::Yes, *parent_tab, AK::move(page_index));
    } else {
        for (size_t i = 0; i < initial_urls.size(); ++i) {
            new_tab_from_url(initial_urls[i], (i == 0) ? Web::HTML::ActivateTab::Yes : Web::HTML::ActivateTab::No);
        }
    }

    m_tabs_container->set_new_tab_action(m_new_tab_action);

    auto* main_widget = new QWidget(this);
    auto* main_layout = new QVBoxLayout(main_widget);
    main_layout->setContentsMargins(0, 0, 0, 0);
    main_layout->setSpacing(0);
    main_layout->addWidget(m_tabs_container, 1);

    m_devtools_banner = new DevToolsBanner(main_widget);
    connect(m_devtools_banner, &DevToolsBanner::disable_requested, this, [] {
        MUST(WebView::Application::the().toggle_devtools_enabled());
    });
    m_devtools_banner->hide();
    main_layout->addWidget(m_devtools_banner);

    setCentralWidget(main_widget);
    setContextMenuPolicy(Qt::PreventContextMenu);

    if (browser_options.devtools_port.has_value())
        on_devtools_enabled();
}

BrowserWindow::~BrowserWindow()
{
    qApp->removeEventFilter(this);
}

void BrowserWindow::update_tabs_display()
{
    auto const& settings = Application::settings().tab_settings();
    m_tabs_container->set_vertical_tabs_enabled(settings.vertical_tabs_enabled);
    m_tabs_container->set_vertical_tabs_expanded(settings.vertical_tabs_expanded);
    m_tabs_container->set_vertical_tabs_expand_on_hover(settings.vertical_tabs_expand_on_hover);
}

void BrowserWindow::rebuild_bookmarks_menu()
{
    repopulate_application_menu(*m_bookmarks_menu, *this, Application::the().bookmarks_menu());

    for_each_tab([](Tab& tab) {
        tab.bookmarks_bar().rebuild();
    });
}

void BrowserWindow::update_bookmarks_bar_display(bool show_bookmarks_bar)
{
    for_each_tab([&](Tab& tab) {
        if (tab.view().is_fullscreen() == Web::ViewportIsFullscreen::No)
            tab.bookmarks_bar().setVisible(show_bookmarks_bar);
    });
}

void BrowserWindow::on_devtools_enabled()
{
    m_devtools_banner->set_port(WebView::Application::browser_options().devtools_port.value_or(0));
    m_devtools_banner->show();
}

void BrowserWindow::on_devtools_disabled()
{
    m_devtools_banner->hide();
}

Tab& BrowserWindow::new_tab_from_url(URL::URL const& url, Web::HTML::ActivateTab activate_tab)
{
    auto& tab = create_new_tab(activate_tab);
    tab.navigate(url);
    return tab;
}

Tab& BrowserWindow::new_child_tab(Web::HTML::ActivateTab activate_tab, Tab& parent, Optional<u64> page_index)
{
    return create_new_tab(activate_tab, parent, page_index);
}

Tab& BrowserWindow::create_new_tab(Web::HTML::ActivateTab activate_tab, Tab& parent, Optional<u64> page_index)
{
    if (!page_index.has_value())
        return create_new_tab(activate_tab);

    auto* tab = new Tab(this, parent.view().client(), page_index.value());

    // FIXME: Merge with other overload
    if (m_current_tab == nullptr) {
        set_current_tab(tab);
    }

    m_tabs_container->add_tab(tab, "New Tab");
    if (activate_tab == Web::HTML::ActivateTab::Yes)
        m_tabs_container->set_current_tab(tab);

    initialize_tab(tab);
    return *tab;
}

FullscreenMode& BrowserWindow::fullscreen_mode()
{
    return *m_fullscreen_mode;
}

Tab& BrowserWindow::create_new_tab(Web::HTML::ActivateTab activate_tab)
{
    auto* tab = new Tab(this);

    if (m_current_tab == nullptr) {
        set_current_tab(tab);
    }

    m_tabs_container->add_tab(tab, "New Tab");
    if (activate_tab == Web::HTML::ActivateTab::Yes)
        m_tabs_container->set_current_tab(tab);

    initialize_tab(tab);

    return *tab;
}

void BrowserWindow::initialize_tab(Tab* tab)
{
    QObject::connect(tab, &Tab::title_changed, this, &BrowserWindow::tab_title_changed);
    QObject::connect(tab, &Tab::favicon_changed, this, &BrowserWindow::tab_favicon_changed);
    QObject::connect(tab, &Tab::audio_play_state_changed, this, &BrowserWindow::tab_audio_play_state_changed);

    QObject::connect(&tab->view(), &WebContentView::urls_dropped, this, [this](auto& urls) {
        VERIFY(urls.size());
        m_current_tab->navigate(ak_url_from_qurl(urls[0]));

        for (qsizetype i = 1; i < urls.size(); ++i)
            new_tab_from_url(ak_url_from_qurl(urls[i]), Web::HTML::ActivateTab::No);
    });

    tab->view().on_new_web_view = [this, tab](auto activate_tab, Web::HTML::WebViewHints hints, Optional<u64> page_index) {
        if (hints.popup) {
            WindowConfiguration configuration {
                .x = hints.screen_x,
                .y = hints.screen_y,
                .width = hints.width,
                .height = hints.height,
            };
            auto& window = Application::the().new_window({}, configuration, IsPopupWindow::Yes, tab, AK::move(page_index));
            return window.current_tab()->view().handle();
        }
        auto& new_tab = new_child_tab(activate_tab, *tab, page_index);
        return new_tab.view().handle();
    };

    initialize_tab_buttons(tab);
}

void BrowserWindow::uninitialize_tab(Tab* tab)
{
    QObject::disconnect(tab, nullptr, this, nullptr);
    QObject::disconnect(&tab->view(), nullptr, this, nullptr);
}

void BrowserWindow::adopt_tab(Tab& tab, int index)
{
    index = clamp(index, 0, m_tabs_container->count());

    tab.set_window(*this);
    m_tabs_container->insert_tab(index, &tab, "New Tab");
    initialize_tab(&tab);
    tab_title_changed(index, tab.title());

    tab.view().set_device_pixel_ratio(m_device_pixel_ratio);
    tab.view().set_display_metadata(m_display_id, m_refresh_rate);

    m_tabs_container->set_current_tab(&tab);
}

void BrowserWindow::move_tab_to_window(int index, BrowserWindow& target_window, int target_index)
{
    if (index < 0 || index >= m_tabs_container->count())
        return;

    if (&target_window == this) {
        if (target_index > index)
            --target_index;
        target_index = clamp(target_index, 0, m_tabs_container->count() - 1);
        move_tab(index, target_index);
        return;
    }

    auto* tab = m_tabs_container->tab(index);
    uninitialize_tab(tab);
    m_tabs_container->take_tab(index);
    if (m_current_tab == tab)
        set_current_tab(m_tabs_container->count() > 0 ? m_tabs_container->tab(m_tabs_container->current_index()) : nullptr);

    target_window.adopt_tab(*tab, target_index);

    if (m_tabs_container->count() == 0) {
        m_should_record_closed_window_on_close = false;
        close();
    }
}

void BrowserWindow::detach_tab_to_new_window(int index, QPoint global_position)
{
    if (index < 0 || index >= m_tabs_container->count())
        return;

    WindowConfiguration configuration {
        .x = Web::DevicePixels { global_position.x() - 160 },
        .y = Web::DevicePixels { global_position.y() - 18 },
        .width = Web::DevicePixels { width() },
        .height = Web::DevicePixels { height() },
        .maximized = isMaximized(),
    };

    auto& window = Application::the().new_window({}, configuration);
    move_tab_to_window(index, window, 0);
}

void BrowserWindow::set_current_tab(Tab* tab)
{
    if (tab == m_current_tab)
        return;

    if (m_current_tab)
        m_current_tab->view().set_system_visibility_state(Web::HTML::VisibilityState::Hidden);

    m_current_tab = tab;

    if (m_current_tab)
        m_current_tab->view().set_system_visibility_state(Web::HTML::VisibilityState::Visible);

    WebView::Application::the().update_bookmark_action_for_current_web_view();
}

void BrowserWindow::activate_tab(int index)
{
    m_tabs_container->set_current_index(index);
}

void BrowserWindow::definitely_close_tab(int index)
{
    auto* tab = m_tabs_container->tab(index);
    auto url = tab->view().url();
    m_tabs_container->remove_tab(index);
    Application::history_store().record_closed_tab(url);
    Application::the().update_reopen_recently_closed_actions();
    tab->deleteLater();

    if (m_tabs_container->count() == 0) {
        m_should_record_closed_window_on_close = false;
        close();
    }
}

void BrowserWindow::update_reopen_recently_closed_action()
{
    if (!m_reopen_recently_closed_tab_action)
        return;

    auto recently_closed_entry = Application::history_store().most_recently_closed_entry();
    m_reopen_recently_closed_tab_action->setText(reopen_recently_closed_action_text(recently_closed_entry));
    m_reopen_recently_closed_tab_action->setEnabled(recently_closed_entry.has_value());
}

void BrowserWindow::move_tab(int old_index, int new_index)
{
    m_tabs_container->tab_bar()->moveTab(old_index, new_index);
}

void BrowserWindow::open_file()
{
    m_current_tab->open_file();
}

void BrowserWindow::request_to_close_tab(int index)
{
    auto* tab = m_tabs_container->tab(index);
    tab->request_close();
}

void BrowserWindow::request_to_close_current_tab()
{
    request_to_close_tab(m_tabs_container->current_index());
}

int BrowserWindow::tab_index(Tab* tab)
{
    return m_tabs_container->index_of(tab);
}

void BrowserWindow::device_pixel_ratio_changed(qreal dpi)
{
    m_device_pixel_ratio = dpi;
    for_each_tab([this](auto& tab) {
        tab.view().set_device_pixel_ratio(m_device_pixel_ratio);
    });
}

bool BrowserWindow::connect_window_screen_changed_signal()
{
    auto* window = windowHandle();
    if (!window)
        return false;
    if (m_window_screen_changed_signal_window == window)
        return true;

    disconnect_window_screen_changed_signal();

    m_window_screen_changed_signal_window = window;
    QObject::connect(window, &QWindow::screenChanged, this, [this](QScreen* screen) {
        screen_changed(screen);
    });
    screen_changed(window->screen());
    return true;
}

void BrowserWindow::disconnect_window_screen_changed_signal()
{
    if (!m_window_screen_changed_signal_window)
        return;

    QObject::disconnect(m_window_screen_changed_signal_window, &QWindow::screenChanged, this, nullptr);
    m_window_screen_changed_signal_window = nullptr;
}

void BrowserWindow::connect_screen_signals(QScreen* screen)
{
    if (!screen)
        return;

    if (should_use_screen_signal_for_dpi_changes())
        QObject::connect(screen, &QScreen::logicalDotsPerInchChanged, this, &BrowserWindow::device_pixel_ratio_changed);
    QObject::connect(screen, &QScreen::refreshRateChanged, this, &BrowserWindow::refresh_rate_changed);
}

void BrowserWindow::disconnect_screen_signals(QScreen* screen)
{
    if (!screen)
        return;

    QObject::disconnect(screen, &QScreen::logicalDotsPerInchChanged, this, &BrowserWindow::device_pixel_ratio_changed);
    QObject::disconnect(screen, &QScreen::refreshRateChanged, this, &BrowserWindow::refresh_rate_changed);
}

void BrowserWindow::screen_changed(QScreen* screen)
{
    if (m_current_screen != screen) {
        disconnect_screen_signals(m_current_screen);
        m_current_screen = screen;
        connect_screen_signals(m_current_screen);
    }

    if (m_device_pixel_ratio != devicePixelRatio())
        device_pixel_ratio_changed(devicePixelRatio());

    auto display_id = display_id_for_screen(m_current_screen);
    auto refresh_rate = m_current_screen ? m_current_screen->refreshRate() : m_refresh_rate;
    if (m_display_id != display_id || m_refresh_rate != refresh_rate)
        display_metadata_changed(display_id, refresh_rate);
}

void BrowserWindow::refresh_rate_changed(qreal refresh_rate)
{
    display_metadata_changed(m_display_id, refresh_rate);
}

void BrowserWindow::display_metadata_changed(Optional<u64> display_id, qreal refresh_rate)
{
    m_display_id = display_id;
    m_refresh_rate = refresh_rate;
    for_each_tab([this](auto& tab) {
        tab.view().set_display_metadata(m_display_id, m_refresh_rate);
    });
}

void BrowserWindow::tab_title_changed(int index, QString const& title)
{
    // NOTE: Qt uses ampersands for shortcut keys in tab titles, so we need to escape them.
    QString title_escaped = title;
    title_escaped.replace("&", "&&");

    m_tabs_container->set_tab_text(index, title_escaped);
    m_tabs_container->set_tab_tooltip(index, title);

    if (m_tabs_container->current_index() == index)
        setWindowTitle(QString("%1 - Ladybird").arg(title));
}

void BrowserWindow::tab_favicon_changed(int index, QIcon const& icon)
{
    if (index < 0)
        return;
    m_tabs_container->set_tab_icon(index, icon);
}

void BrowserWindow::initialize_tab_buttons(Tab* tab)
{
    auto index = m_tabs_container->index_of(tab);
    m_tabs_container->set_tab_icon(index, tab->tab_icon());

    auto* close_button = new TabBarButton(create_chrome_icon(ChromeIcon::TabClose, palette()));
    close_button->setToolTip("Close Tab");

    connect(close_button, &QPushButton::clicked, this, [this, tab]() {
        auto index = m_tabs_container->index_of(tab);
        request_to_close_tab(index);
    });

    m_tabs_container->tab_bar()->setTabButton(index, AUDIO_STATE_BUTTON_POSITION, nullptr);
    m_tabs_container->tab_bar()->setTabButton(index, TAB_CLOSE_BUTTON_POSITION, close_button);
    m_tabs_container->update_tab_button_visibility();
}

void BrowserWindow::update_tab_button_icons()
{
    for (int index = 0; index < m_tabs_container->count(); ++index) {
        if (auto* button = m_tabs_container->tab_bar()->tabButton(index, TAB_CLOSE_BUTTON_POSITION)) {
            if (auto* tab_bar_button = qobject_cast<TabBarButton*>(button))
                tab_bar_button->setIcon(create_chrome_icon(ChromeIcon::TabClose, palette()));
        }

        if (auto* button = m_tabs_container->tab_bar()->tabButton(index, AUDIO_STATE_BUTTON_POSITION)) {
            if (auto* tab_bar_button = qobject_cast<TabBarButton*>(button))
                tab_bar_button->setIcon(icon_for_page_mute_state(*m_tabs_container->tab(index)));
        }
    }
}

void BrowserWindow::create_menu_bar_window_controls()
{
    if (use_left_traffic_light_window_controls())
        return;

    auto window_control_buttons = create_window_control_buttons(*menuBar(), "LadybirdMenuBarWindowControls", { 16, 16 }, { 40, 30 });
    m_menu_bar_window_controls = window_control_buttons.container;
    m_menu_bar_minimize_window_button = window_control_buttons.minimize;
    m_menu_bar_maximize_window_button = window_control_buttons.maximize;
    m_menu_bar_close_window_button = window_control_buttons.close;
    menuBar()->setCornerWidget(m_menu_bar_window_controls, Qt::TopRightCorner);

    connect(m_menu_bar_minimize_window_button, &QToolButton::clicked, this, [this] {
        showMinimized();
    });
    connect(m_menu_bar_maximize_window_button, &QToolButton::clicked, this, [this] {
        toggle_window_maximized();
    });
    connect(m_menu_bar_close_window_button, &QToolButton::clicked, this, [this] {
        close();
    });

    update_menu_bar_window_control_icons();
}

void BrowserWindow::update_menu_bar_style()
{
    menuBar()->setStyleSheet(ChromeStyle::menu_bar_style_sheet(palette()));
}

void BrowserWindow::update_menu_bar_visibility(bool show_menubar)
{
    menuBar()->setVisible(show_menubar);
    if (m_menu_bar_window_controls)
        m_menu_bar_window_controls->setVisible(show_menubar);
    m_tabs_container->set_window_controls_visible(!show_menubar);
}

void BrowserWindow::update_menu_bar_window_control_icons()
{
    if (!m_menu_bar_minimize_window_button || !m_menu_bar_maximize_window_button || !m_menu_bar_close_window_button)
        return;

    auto is_maximized = this->isMaximized();
    m_menu_bar_minimize_window_button->setIcon(create_chrome_icon(ChromeIcon::WindowMinimize, palette()));
    m_menu_bar_maximize_window_button->setIcon(create_chrome_icon(is_maximized ? ChromeIcon::WindowRestore : ChromeIcon::WindowMaximize, palette()));
    m_menu_bar_maximize_window_button->setToolTip(is_maximized ? "Restore" : "Maximize");
    m_menu_bar_close_window_button->setIcon(create_chrome_icon(ChromeIcon::WindowClose, palette()));
}

void BrowserWindow::toggle_window_maximized()
{
    if (isMaximized())
        showNormal();
    else
        showMaximized();
    update_menu_bar_window_control_icons();
}

bool BrowserWindow::start_window_move()
{
    auto* handle = windowHandle();
    if (!handle)
        return false;
#if defined(AK_OS_MACOS)
    if (start_appkit_window_drag(*this))
        return true;
#endif
    return handle->startSystemMove();
}

void BrowserWindow::tab_audio_play_state_changed(int index, Web::HTML::AudioPlayState play_state)
{
    auto* tab = m_tabs_container->tab(index);

    switch (play_state) {
    case Web::HTML::AudioPlayState::Paused:
        if (tab->view().page_mute_state() == Web::HTML::MuteState::Unmuted) {
            m_tabs_container->tab_bar()->setTabButton(index, AUDIO_STATE_BUTTON_POSITION, nullptr);
            m_tabs_container->update_tab_button_visibility();
        }
        break;

    case Web::HTML::AudioPlayState::Playing:
        auto* button = new TabBarButton(icon_for_page_mute_state(*tab));
        button->setToolTip(tool_tip_for_page_mute_state(*tab));
        button->setObjectName("LadybirdAudioState");

        connect(button, &QPushButton::clicked, this, [this, tab]() {
            tab->view().toggle_page_mute_state();
            auto index = tab_index(tab);

            switch (tab->view().audio_play_state()) {
            case Web::HTML::AudioPlayState::Paused:
                m_tabs_container->tab_bar()->setTabButton(index, AUDIO_STATE_BUTTON_POSITION, nullptr);
                m_tabs_container->update_tab_button_visibility();
                break;
            case Web::HTML::AudioPlayState::Playing:
                auto* button = m_tabs_container->tab_bar()->tabButton(index, AUDIO_STATE_BUTTON_POSITION);
                as<TabBarButton>(button)->setIcon(icon_for_page_mute_state(*tab));
                button->setToolTip(tool_tip_for_page_mute_state(*tab));
                break;
            }
        });

        m_tabs_container->tab_bar()->setTabButton(index, AUDIO_STATE_BUTTON_POSITION, button);
        m_tabs_container->update_tab_button_visibility();
        break;
    }
}

QIcon BrowserWindow::icon_for_page_mute_state(Tab& tab) const
{
    switch (tab.view().page_mute_state()) {
    case Web::HTML::MuteState::Muted:
        return create_chrome_icon(ChromeIcon::VolumeMuted, palette());
    case Web::HTML::MuteState::Unmuted:
        return create_chrome_icon(ChromeIcon::Volume, palette());
    }

    VERIFY_NOT_REACHED();
}

QString BrowserWindow::tool_tip_for_page_mute_state(Tab& tab) const
{
    switch (tab.view().page_mute_state()) {
    case Web::HTML::MuteState::Muted:
        return "Unmute tab";
    case Web::HTML::MuteState::Unmuted:
        return "Mute tab";
    }

    VERIFY_NOT_REACHED();
}

void BrowserWindow::open_next_tab()
{
    if (m_tabs_container->count() <= 1)
        return;

    auto next_index = m_tabs_container->current_index() + 1;
    if (next_index >= m_tabs_container->count())
        next_index = 0;
    m_tabs_container->set_current_index(next_index);
}

void BrowserWindow::open_previous_tab()
{
    if (m_tabs_container->count() <= 1)
        return;

    auto next_index = m_tabs_container->current_index() - 1;
    if (next_index < 0)
        next_index = m_tabs_container->count() - 1;
    m_tabs_container->set_current_index(next_index);
}

void BrowserWindow::show_find_in_page()
{
    if (!m_current_tab)
        return;

    m_current_tab->show_find_in_page();
}

void BrowserWindow::set_window_rect(Optional<Web::DevicePixels> x, Optional<Web::DevicePixels> y, Optional<Web::DevicePixels> width, Optional<Web::DevicePixels> height)
{
    x = x.value_or(0);
    y = y.value_or(0);
    if (!width.has_value() || width.value() == 0)
        width = 800;
    if (!height.has_value() || height.value() == 0)
        height = 600;

    setGeometry(x.value().value(), y.value().value(), width.value().value(), height.value().value());
}

void BrowserWindow::enter_fullscreen()
{
    m_tabs_container->set_tab_bar_visible(false);
    current_tab()->bookmarks_bar().setVisible(false);

    m_restore_to_maximized = isMaximized();
    showFullScreen();
}

void BrowserWindow::exit_fullscreen()
{
    m_tabs_container->set_tab_bar_visible(true);
    current_tab()->bookmarks_bar().setVisible(WebView::Application::settings().show_bookmarks_bar());

    if (m_restore_to_maximized)
        showMaximized();
    else
        showNormal();
}

bool BrowserWindow::event(QEvent* event)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    if (event->type() == QEvent::DevicePixelRatioChange) {
        if (m_device_pixel_ratio != devicePixelRatio())
            device_pixel_ratio_changed(devicePixelRatio());
    }
#endif
    if (event->type() == QEvent::WinIdChange)
        connect_window_screen_changed_signal();
    if (event->type() == QEvent::PlatformSurface) {
        auto* platform_surface_event = static_cast<QPlatformSurfaceEvent*>(event);
        if (platform_surface_event->surfaceEventType() == QPlatformSurfaceEvent::SurfaceCreated) {
            connect_window_screen_changed_signal();
#if defined(AK_OS_MACOS)
            QTimer::singleShot(0, this, [this] {
                update_window_corners();
            });
#endif
        } else if (platform_surface_event->surfaceEventType() == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed) {
            disconnect_window_screen_changed_signal();
        }
    }
    if (event->type() == QEvent::ScreenChangeInternal)
        screen_changed(screen());

    if (event->type() == QEvent::WindowActivate)
        Application::the().set_active_window(*this);
    else if (event->type() == QEvent::WindowDeactivate || event->type() == QEvent::Hide)
        clear_resize_cursor();

    return QMainWindow::event(event);
}

bool BrowserWindow::eventFilter(QObject* object, QEvent* event)
{
    auto* widget = as_if<QWidget>(object);
    if (!widget || widget->window() != this)
        return QMainWindow::eventFilter(object, event);

    auto const is_button = qobject_cast<QAbstractButton*>(object) != nullptr;

    if (is_button && (event->type() == QEvent::Enter || event->type() == QEvent::MouseMove || event->type() == QEvent::Leave)) {
        clear_resize_cursor();
    } else if (event->type() == QEvent::Enter) {
        update_resize_cursor(mapFromGlobal(QCursor::pos()));
    } else if (event->type() == QEvent::MouseMove) {
        auto* mouse_event = static_cast<QMouseEvent*>(event);
        update_resize_cursor(widget->mapTo(this, mouse_event->position().toPoint()));
    } else if (event->type() == QEvent::Leave) {
        auto position = mapFromGlobal(QCursor::pos());
        if (rect().contains(position))
            update_resize_cursor(position);
        else
            clear_resize_cursor();
    } else if (event->type() != QEvent::MouseButtonPress && event->type() != QEvent::MouseButtonDblClick) {
        return QMainWindow::eventFilter(object, event);
    }

    if (event->type() != QEvent::MouseButtonPress && event->type() != QEvent::MouseButtonDblClick)
        return QMainWindow::eventFilter(object, event);

    if (is_button)
        return QMainWindow::eventFilter(object, event);

    auto* mouse_event = static_cast<QMouseEvent*>(event);
    if (mouse_event->button() != Qt::LeftButton)
        return QMainWindow::eventFilter(object, event);

    auto position = widget->mapTo(this, mouse_event->position().toPoint());
    if (event->type() == QEvent::MouseButtonPress && !isMaximized() && !isFullScreen()) {
        auto edges = resize_edges_for_position(position);
        auto* handle = windowHandle();
        if (edges != Qt::Edges {} && handle && handle->startSystemResize(edges))
            return true;
    }

    auto is_empty_window_drag_region = widget->property(WINDOW_DRAG_REGION_PROPERTY).toBool()
        && widget->childAt(mouse_event->position().toPoint()) == nullptr;
    if (is_empty_window_drag_region && !isFullScreen()) {
        if (event->type() == QEvent::MouseButtonDblClick) {
            toggle_window_maximized();
            return true;
        }

        if (start_window_move())
            return true;
    }

    if (isFullScreen() || widget != menuBar() || menuBar()->actionAt(mouse_event->position().toPoint()) != nullptr)
        return QMainWindow::eventFilter(object, event);

    if (event->type() == QEvent::MouseButtonDblClick) {
        toggle_window_maximized();
        return true;
    }

    if (start_window_move())
        return true;

    return QMainWindow::eventFilter(object, event);
}

Qt::Edges BrowserWindow::resize_edges_for_position(QPoint const& position) const
{
    static constexpr int resize_border_width = 6;

    Qt::Edges edges;
    if (position.x() <= resize_border_width)
        edges |= Qt::LeftEdge;
    if (position.x() >= width() - resize_border_width)
        edges |= Qt::RightEdge;
    if (position.y() <= resize_border_width)
        edges |= Qt::TopEdge;
    if (position.y() >= height() - resize_border_width)
        edges |= Qt::BottomEdge;

    return edges;
}

Optional<Qt::CursorShape> BrowserWindow::resize_cursor_for_edges(Qt::Edges edges) const
{
    if (edges == Qt::Edges {})
        return {};

    if ((edges & Qt::TopEdge && edges & Qt::LeftEdge) || (edges & Qt::BottomEdge && edges & Qt::RightEdge))
        return Qt::SizeFDiagCursor;
    if ((edges & Qt::TopEdge && edges & Qt::RightEdge) || (edges & Qt::BottomEdge && edges & Qt::LeftEdge))
        return Qt::SizeBDiagCursor;
    if (edges & Qt::LeftEdge || edges & Qt::RightEdge)
        return Qt::SizeHorCursor;
    if (edges & Qt::TopEdge || edges & Qt::BottomEdge)
        return Qt::SizeVerCursor;

    return {};
}

void BrowserWindow::update_resize_cursor(QPoint const& position)
{
    if (isMaximized() || isFullScreen() || !rect().contains(position)) {
        clear_resize_cursor();
        return;
    }

    auto cursor_shape = resize_cursor_for_edges(resize_edges_for_position(position));
    if (!cursor_shape.has_value()) {
        clear_resize_cursor();
        return;
    }

    if (m_resize_cursor_active)
        QApplication::changeOverrideCursor(*cursor_shape);
    else {
        QApplication::setOverrideCursor(*cursor_shape);
        m_resize_cursor_active = true;
    }
}

void BrowserWindow::clear_resize_cursor()
{
    if (!m_resize_cursor_active)
        return;

    QApplication::restoreOverrideCursor();
    m_resize_cursor_active = false;
}

void BrowserWindow::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    update_window_corners();

    for_each_tab([&](auto& tab) {
        tab.view().set_window_size({ width(), height() });
    });
}

void BrowserWindow::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::PaletteChange) {
        update_menu_bar_style();
        update_menu_bar_window_control_icons();
        update_tab_button_icons();
    } else if (event->type() == QEvent::WindowStateChange) {
        update_menu_bar_window_control_icons();
        m_tabs_container->update_window_button_icons();
        update_window_corners();

        QWindowStateChangeEvent* stateChangeEvent = static_cast<QWindowStateChangeEvent*>(event);
        bool was_fullscreen = stateChangeEvent->oldState() & Qt::WindowFullScreen;
        bool is_fullscreen = windowState() & Qt::WindowFullScreen;

        if (is_fullscreen && !was_fullscreen) {
            m_fullscreen_mode->entered_fullscreen();
            if (m_fullscreen_mode->is_api_fullscreen())
                view().set_is_fullscreen(Web::ViewportIsFullscreen::Yes);
        } else if (!is_fullscreen && was_fullscreen) {
            view().set_is_fullscreen(Web::ViewportIsFullscreen::No);
        }
    }
    QWidget::changeEvent(event);
}

void BrowserWindow::config_variable_changed(WebView::ConfigVariableID variable)
{
    if (variable == WebView::ConfigVariableID::UseRoundedWindowCorners)
        update_window_corners();
}

void BrowserWindow::update_window_corners()
{
#if defined(AK_OS_MACOS)
    auto should_use_rounded_corners = WebView::Application::settings().config_variable_as_bool(WebView::ConfigVariableID::UseRoundedWindowCorners);
    auto should_round_window = should_use_rounded_corners && !isFullScreen();

    clearMask();
    set_rounded_window_corners(*this, should_round_window, WINDOW_CORNER_RADIUS, ChromeStyle::chrome_background(palette()));
#endif
}

void BrowserWindow::moveEvent(QMoveEvent* event)
{
    QWidget::moveEvent(event);

    for_each_tab([&](auto& tab) {
        tab.view().set_window_position({ x(), y() });
    });
}

void BrowserWindow::wheelEvent(QWheelEvent* event)
{
    if (!m_current_tab)
        return;

    if ((event->modifiers() & Qt::ControlModifier) != 0) {
        if (event->angleDelta().y() > 0)
            m_current_tab->view().zoom_in();
        else if (event->angleDelta().y() < 0)
            m_current_tab->view().zoom_out();
    }
}

void BrowserWindow::closeEvent(QCloseEvent* event)
{
    clear_resize_cursor();

    Optional<Vector<URL::URL>> recently_closed_window_urls;
    size_t recently_closed_window_active_tab_index { 0 };
    if (m_should_record_closed_window_on_close && m_tabs_container->count() > 0) {
        recently_closed_window_urls = recently_closed_urls_for_window(*m_tabs_container);
        recently_closed_window_active_tab_index = static_cast<size_t>(m_tabs_container->current_index());
    }

    if (m_is_popup_window == IsPopupWindow::No) {
        Settings::the()->set_last_position(pos());
        Settings::the()->set_last_size(size());
        Settings::the()->set_is_maximized(isMaximized());
    }

    QObject::deleteLater();

    QMainWindow::closeEvent(event);

    if (event->isAccepted() && recently_closed_window_urls.has_value()) {
        Application::history_store().record_closed_window(recently_closed_window_urls.release_value(), recently_closed_window_active_tab_index);
        Application::the().update_reopen_recently_closed_actions();
    }
}

}
