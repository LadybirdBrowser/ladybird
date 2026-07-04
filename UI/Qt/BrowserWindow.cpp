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
#include <QPainter>
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
static constexpr int WINDOW_RESIZE_BORDER_WIDTH = 6;
static constexpr int WINDOW_RESIZE_CORNER_WIDTH = WINDOW_RESIZE_BORDER_WIDTH * 2;

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

static Vector<URL::URL> recently_closed_urls_for_window(TabWidget const& tabs_container)
{
    Vector<URL::URL> urls;
    urls.ensure_capacity(tabs_container.count());

    for (int index = 0; index < tabs_container.count(); ++index)
        urls.append(tabs_container.tab(index)->view().url());

    return urls;
}

static int visible_browser_window_count()
{
    int count = 0;
    for (auto* widget : QApplication::topLevelWidgets()) {
        if (as_if<BrowserWindow>(widget) && widget->isVisible())
            ++count;
    }
    return count;
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
        maybe_animate_show_exit_button(m_window->mapFromGlobal(mouse_event->globalPosition().toPoint()));
    }

    return QObject::eventFilter(obj, event);
}

ExitFullscreenButton::ExitFullscreenButton(QWidget* parent)
    : QPushButton("Exit fullscreen", parent)
{
#if defined(AK_OS_MACOS)
    // The web content view is a native QRhiWidget on macOS, so this overlay must also be native to remain above it.
    setAttribute(Qt::WA_NativeWindow);
#endif
    setStyleSheet("background-color:rgb(55, 99, 129); color: white; padding: 10px; border-radius: 5px;");
    adjustSize();
    hide();
    m_widget_animation = new QPropertyAnimation(this, "pos", this);
}

void ExitFullscreenButton::animate_show()
{
    if (isVisible())
        return;

    show();
    raise();

    auto const container_size = screen() ? screen()->geometry().size() : (parentWidget() ? parentWidget()->size() : QSize {});
    int const destination_x = (container_size.width() - width()) / 2;
    int const destination_y = static_cast<int>(static_cast<float>(container_size.height()) * 0.05);

    m_widget_animation->stop();
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
    auto& application = WebView::Application::the();
    auto const& browser_options = WebView::Application::browser_options();

    setWindowFlag(Qt::FramelessWindowHint, uses_client_side_decorations());
    setAttribute(Qt::WA_OpaquePaintEvent);
    setWindowIcon(app_icon());
    qApp->installEventFilter(this);
    update_window_corners();
    update_appkit_window_resizability();
    update_window_border();

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
    update_menu_bar_visibility();

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

    m_hamburger_menu->addAction(create_application_action(*this, application.open_downloads_page_action(), IncludeActionIcon::No));
    file_menu->addAction(create_application_action(*this, application.open_downloads_page_action(), IncludeActionIcon::No));

    m_hamburger_menu->addSeparator();

    auto* edit_menu = m_hamburger_menu->addMenu("&Edit");
    menuBar()->addMenu(edit_menu);

    edit_menu->addAction(create_application_action(*this, application.cut_selection_action(), IncludeActionIcon::No));
    edit_menu->addAction(create_application_action(*this, application.copy_selection_action(), IncludeActionIcon::No));
    edit_menu->addAction(create_application_action(*this, application.paste_action(), IncludeActionIcon::No));
    edit_menu->addAction(create_application_action(*this, application.select_all_action(), IncludeActionIcon::No));
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
    edit_menu->addAction(create_application_action(*edit_menu, application.open_settings_page_action(), IncludeActionIcon::No));

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

    view_menu->addMenu(create_application_menu(*view_menu, application.zoom_menu()));
    view_menu->addSeparator();

    view_menu->addMenu(create_application_menu(*view_menu, application.color_scheme_menu()));
    view_menu->addMenu(create_application_menu(*view_menu, application.contrast_menu()));
    view_menu->addMenu(create_application_menu(*view_menu, application.motion_menu()));
    view_menu->addSeparator();

    if (show_menubar_option_available())
        view_menu->addAction(create_application_action(*view_menu, application.toggle_menu_bar_action(), IncludeActionIcon::No));

    m_bookmarks_menu = Application::the().qt_bookmarks_menu();
    if (!m_bookmarks_menu)
        m_bookmarks_menu = create_application_menu(*this, application.bookmarks_menu());
    m_hamburger_menu->addMenu(m_bookmarks_menu);
    menuBar()->addMenu(m_bookmarks_menu);

    m_history_menu = create_application_menu(*this, application.history_menu());
    m_hamburger_menu->addMenu(m_history_menu);
    menuBar()->addMenu(m_history_menu);

    auto* inspect_menu = create_application_menu(*m_hamburger_menu, application.inspect_menu());
    m_hamburger_menu->addMenu(inspect_menu);
    menuBar()->addMenu(inspect_menu);

    auto* debug_menu = create_application_menu(*m_hamburger_menu, application.debug_menu());
    m_hamburger_menu->addMenu(debug_menu);
    menuBar()->addMenu(debug_menu);

    auto* help_menu = m_hamburger_menu->addMenu("&Help");
    menuBar()->addMenu(help_menu);

    help_menu->addAction(create_application_action(*help_menu, application.open_about_page_action(), IncludeActionIcon::No));

    m_hamburger_menu->addSeparator();
    file_menu->addSeparator();

    auto* quit_action = new QAction("&Quit", this);
    quit_action->setShortcuts(QKeySequence::keyBindings(QKeySequence::StandardKey::Quit));
    m_hamburger_menu->addAction(quit_action);
    file_menu->addAction(quit_action);
#if defined(AK_OS_MACOS)
    QObject::connect(quit_action, &QAction::triggered, this, [] {
        Application::the().quit();
    });
#else
    QObject::connect(quit_action, &QAction::triggered, this, &QMainWindow::close);
#endif

    QObject::connect(m_new_tab_action, &QAction::triggered, this, [] {
        Application::the().open_new_tab();
    });
    QObject::connect(m_new_window_action, &QAction::triggered, this, [] {
        Application::the().open_new_window();
    });
    QObject::connect(m_reopen_recently_closed_tab_action, &QAction::triggered, this, [] {
        Application::the().reopen_recently_closed_tab();
    });
    QObject::connect(open_file_action, &QAction::triggered, this, [] {
        Application::the().open_file();
    });

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
            QWidget* focus_widget = &tab->view();
            if (auto* tab_focus_widget = tab->focusWidget(); tab_focus_widget && tab->isAncestorOf(tab_focus_widget))
                focus_widget = tab_focus_widget;
#if defined(AK_OS_MACOS)
            make_appkit_window_first_responder(*focus_widget);
#endif
            focus_widget->setFocus();
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
    m_tabs_container->set_vertical_tabs_position(settings.vertical_tabs_position);
}

void BrowserWindow::rebuild_bookmarks_menu()
{
    if (m_bookmarks_menu != Application::the().qt_bookmarks_menu())
        repopulate_application_menu(*m_bookmarks_menu, *this, Application::the().bookmarks_menu());

    for_each_tab([](Tab& tab) {
        tab.bookmarks_bar().rebuild();
    });
}

void BrowserWindow::show_bookmarks_bar_changed()
{
    auto show_bookmarks_bar = WebView::Application::settings().show_bookmarks_bar();

    for_each_tab([&](Tab& tab) {
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

bool BrowserWindow::uses_client_side_decorations()
{
    return !WebView::Application::settings().config_variable_as_bool(WebView::ConfigVariableID::UseServerSideWindowDecorations);
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

bool BrowserWindow::activate_tab_with_url(URL::URL const& url)
{
    for (int index = 0; index < m_tabs_container->count(); ++index) {
        auto* tab = m_tabs_container->tab(index);
        if (tab && tab->view().url() == url) {
            m_tabs_container->set_current_index(index);
            return true;
        }
    }
    return false;
}

bool BrowserWindow::definitely_close_tab(int index)
{
    if (m_tabs_container->count() == 1 && Application::the().file_downloader().has_active_downloads() && visible_browser_window_count() <= 1) {
        if (!Application::the().confirm_cancel_active_downloads(this))
            return false;
    }

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

    return true;
}

void BrowserWindow::update_reopen_recently_closed_action()
{
    if (!m_reopen_recently_closed_tab_action)
        return;

    auto recently_closed_entry = Application::history_store().most_recently_closed_entry();
    m_reopen_recently_closed_tab_action->setText("&Reopen Recently Closed Tab");
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

void BrowserWindow::update_menu_bar_visibility()
{
    auto show_menu_bar = show_menubar_option_available() && WebView::Application::settings().show_menu_bar();
    menuBar()->setVisible(show_menu_bar);

    if (m_menu_bar_window_controls)
        m_menu_bar_window_controls->setVisible(show_menu_bar && uses_client_side_decorations());
    m_tabs_container->set_window_controls_visible(!show_menu_bar && uses_client_side_decorations());
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

void BrowserWindow::update_window_decoration_state()
{
    clear_resize_cursor();

    auto should_be_frameless = uses_client_side_decorations();
    auto is_frameless = windowFlags().testFlag(Qt::FramelessWindowHint);

    if (is_frameless != should_be_frameless) {
        auto was_visible = isVisible();
        auto was_fullscreen = isFullScreen();
        auto was_maximized = isMaximized();

        setWindowFlag(Qt::FramelessWindowHint, should_be_frameless);

        if (was_visible) {
            if (was_fullscreen)
                showFullScreen();
            else if (was_maximized)
                showMaximized();
            else
                show();
        }
    }

    update_appkit_window_resizability();
    update_menu_bar_visibility();
    update_window_border();
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
                update_appkit_window_resizability();
            });
#endif
        } else if (platform_surface_event->surfaceEventType() == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed) {
            disconnect_window_screen_changed_signal();
        }
    }
    if (event->type() == QEvent::ScreenChangeInternal)
        screen_changed(screen());

    if (event->type() == QEvent::WindowActivate) {
        Application::the().set_active_window(*this);
        QTimer::singleShot(0, this, [this] {
            refresh_resize_cursor_at_current_position(true);
        });
        QTimer::singleShot(50, this, [this] {
            refresh_resize_cursor_at_current_position(true);
        });
    } else if (event->type() == QEvent::WindowDeactivate || event->type() == QEvent::Hide) {
        clear_resize_cursor();
    }

    return QMainWindow::event(event);
}

bool BrowserWindow::eventFilter(QObject* object, QEvent* event)
{
    if (auto* native_window = as_if<QWindow>(object)) {
        if (filter_native_window_event(*native_window, *event))
            return true;
        return QMainWindow::eventFilter(object, event);
    }

    auto* widget = as_if<QWidget>(object);
    if (!widget || widget->window() != this)
        return QMainWindow::eventFilter(object, event);
    if (!uses_client_side_decorations())
        return QMainWindow::eventFilter(object, event);

    if (m_is_resizing_window) {
        if (event->type() == QEvent::MouseMove) {
            auto* mouse_event = static_cast<QMouseEvent*>(event);
            update_window_resize(mouse_event->globalPosition().toPoint());
            return true;
        }
        if (event->type() == QEvent::MouseButtonRelease) {
            finish_window_resize();
            return true;
        }
    }

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
    auto edges = resize_edges_for_position(position);
    if (event->type() == QEvent::MouseButtonPress && !isMaximized() && !isFullScreen()) {
        if (edges != Qt::Edges {}) {
#if defined(AK_OS_MACOS)
            if (start_window_resize(edges, mouse_event->globalPosition().toPoint()))
                return true;
#else
            auto* handle = windowHandle();
            if (handle && handle->startSystemResize(edges))
                return true;
#endif
        }
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

bool BrowserWindow::filter_native_window_event(QWindow& window, QEvent& event)
{
    if (!uses_client_side_decorations())
        return false;

    auto* window_handle = windowHandle();
    if (!window_handle)
        return false;

    bool window_is_embedded_in_this_window = false;
    for (auto const* ancestor = window.parent(); ancestor; ancestor = ancestor->parent()) {
        if (ancestor == window_handle) {
            window_is_embedded_in_this_window = true;
            break;
        }
    }
    if (!window_is_embedded_in_this_window)
        return false;

    switch (event.type()) {
    case QEvent::Enter:
        update_resize_cursor(mapFromGlobal(QCursor::pos()));
        return false;
    case QEvent::MouseMove:
        update_resize_cursor(mapFromGlobal(static_cast<QMouseEvent const&>(event).globalPosition().toPoint()));
        return false;
    case QEvent::Leave: {
        auto position = mapFromGlobal(QCursor::pos());
        if (rect().contains(position))
            update_resize_cursor(position);
        else
            clear_resize_cursor();
        return false;
    }
    case QEvent::MouseButtonPress: {
        auto const& mouse_event = static_cast<QMouseEvent const&>(event);
        if (mouse_event.button() != Qt::LeftButton || isMaximized() || isFullScreen())
            return false;

        auto edges = resize_edges_for_position(mapFromGlobal(mouse_event.globalPosition().toPoint()));
        if (edges == Qt::Edges {})
            return false;

        return window_handle->startSystemResize(edges);
    }
    default:
        return false;
    }
}

bool BrowserWindow::position_is_in_rounded_corner_cutout(QPoint const& position) const
{
#if defined(AK_OS_MACOS)
    auto should_use_rounded_corners = WebView::Application::settings().config_variable_as_bool(WebView::ConfigVariableID::UseRoundedWindowCorners);
    if (!should_use_rounded_corners || isFullScreen())
        return false;

    auto const radius = WINDOW_CORNER_RADIUS;
    auto const x = static_cast<qreal>(position.x());
    auto const y = static_cast<qreal>(position.y());
    auto const right = static_cast<qreal>(width());
    auto const bottom = static_cast<qreal>(height());

    auto is_outside_corner_arc = [radius](qreal dx, qreal dy) {
        return dx * dx + dy * dy > radius * radius;
    };

    auto in_cutout = false;
    if (x < radius && y < radius)
        in_cutout = is_outside_corner_arc(radius - x, radius - y);
    else if (x >= right - radius && y < radius)
        in_cutout = is_outside_corner_arc(x - (right - radius), radius - y);
    else if (x < radius && y >= bottom - radius)
        in_cutout = is_outside_corner_arc(radius - x, y - (bottom - radius));
    else if (x >= right - radius && y >= bottom - radius)
        in_cutout = is_outside_corner_arc(x - (right - radius), y - (bottom - radius));

    return in_cutout;
#else
    (void)position;
#endif
    return false;
}

Qt::Edges BrowserWindow::resize_edges_for_position(QPoint const& position) const
{
    if (position_is_in_rounded_corner_cutout(position))
        return {};

    Qt::Edges edges;
    auto in_left_resize_edge = position.x() <= WINDOW_RESIZE_BORDER_WIDTH;
    auto in_right_resize_edge = position.x() >= width() - WINDOW_RESIZE_BORDER_WIDTH;
    auto in_top_resize_edge = position.y() <= WINDOW_RESIZE_BORDER_WIDTH;
    auto in_bottom_resize_edge = position.y() >= height() - WINDOW_RESIZE_BORDER_WIDTH;
    auto in_left_resize_corner = position.x() <= WINDOW_RESIZE_CORNER_WIDTH;
    auto in_right_resize_corner = position.x() >= width() - WINDOW_RESIZE_CORNER_WIDTH;
    auto in_top_resize_corner = position.y() <= WINDOW_RESIZE_CORNER_WIDTH;
    auto in_bottom_resize_corner = position.y() >= height() - WINDOW_RESIZE_CORNER_WIDTH;

    if (in_left_resize_edge || (in_left_resize_corner && (in_top_resize_corner || in_bottom_resize_corner)))
        edges |= Qt::LeftEdge;
    if (in_right_resize_edge || (in_right_resize_corner && (in_top_resize_corner || in_bottom_resize_corner)))
        edges |= Qt::RightEdge;
    if (in_top_resize_edge || (in_top_resize_corner && (in_left_resize_corner || in_right_resize_corner)))
        edges |= Qt::TopEdge;
    if (in_bottom_resize_edge || (in_bottom_resize_corner && (in_left_resize_corner || in_right_resize_corner)))
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

bool BrowserWindow::start_window_resize(Qt::Edges edges, QPoint const& global_position)
{
    if (edges == Qt::Edges {} || isMaximized() || isFullScreen())
        return false;

    m_is_resizing_window = true;
    m_resize_edges = edges;
    m_resize_start_global_position = global_position;
    m_resize_start_geometry = geometry();
    grabMouse();
    return true;
}

void BrowserWindow::update_window_resize(QPoint const& global_position)
{
    if (!m_is_resizing_window)
        return;

    auto delta = global_position - m_resize_start_global_position;
    auto new_geometry = m_resize_start_geometry;

    if (m_resize_edges & Qt::LeftEdge) {
        auto new_width = qBound(minimumWidth(), m_resize_start_geometry.width() - delta.x(), maximumWidth());
        new_geometry.setX(m_resize_start_geometry.right() - new_width + 1);
    } else if (m_resize_edges & Qt::RightEdge) {
        auto new_width = qBound(minimumWidth(), m_resize_start_geometry.width() + delta.x(), maximumWidth());
        new_geometry.setWidth(new_width);
    }

    if (m_resize_edges & Qt::TopEdge) {
        auto new_height = qBound(minimumHeight(), m_resize_start_geometry.height() - delta.y(), maximumHeight());
        new_geometry.setY(m_resize_start_geometry.bottom() - new_height + 1);
    } else if (m_resize_edges & Qt::BottomEdge) {
        auto new_height = qBound(minimumHeight(), m_resize_start_geometry.height() + delta.y(), maximumHeight());
        new_geometry.setHeight(new_height);
    }

    setGeometry(new_geometry);
}

void BrowserWindow::finish_window_resize()
{
    if (!m_is_resizing_window)
        return;

    m_is_resizing_window = false;
    m_resize_edges = {};
    releaseMouse();
}

void BrowserWindow::update_resize_cursor(QPoint const& position)
{
    if (!uses_client_side_decorations() || isMaximized() || isFullScreen() || !rect().contains(position)) {
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

void BrowserWindow::refresh_resize_cursor_at_current_position(bool force_reapply)
{
    auto position = mapFromGlobal(QCursor::pos());
    auto* child = childAt(position);
    for (auto* object = child; object; object = qobject_cast<QWidget*>(object->parent())) {
        if (qobject_cast<QAbstractButton*>(object)) {
            clear_resize_cursor();
            return;
        }
    }

    if (force_reapply && m_resize_cursor_active) {
        QApplication::restoreOverrideCursor();
        m_resize_cursor_active = false;
    }

    update_resize_cursor(position);
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
        update_window_border();

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

void BrowserWindow::show_menu_bar_changed()
{
    update_menu_bar_visibility();
}

void BrowserWindow::config_variable_changed(WebView::ConfigVariableID variable)
{
    if (variable == WebView::ConfigVariableID::UseRoundedWindowCorners)
        update_window_corners();
    else if (variable == WebView::ConfigVariableID::UseServerSideWindowDecorations)
        update_window_decoration_state();
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

void BrowserWindow::update_appkit_window_resizability()
{
#if defined(AK_OS_MACOS)
    set_appkit_window_resizable(*this, !uses_client_side_decorations());
#endif
}

bool BrowserWindow::should_draw_window_border() const
{
#if defined(AK_OS_MACOS)
    // macOS frameless windows already get rounded corners and a native shadow, so a painted border would clash.
    return false;
#else
    return windowFlags().testFlag(Qt::FramelessWindowHint) && !isFullScreen() && !isMaximized();
#endif
}

void BrowserWindow::update_window_border()
{
    auto border_width = should_draw_window_border() ? 1 : 0;
    setContentsMargins(border_width, border_width, border_width, border_width);
    update();
}

void BrowserWindow::paintEvent(QPaintEvent* event)
{
    QMainWindow::paintEvent(event);

    if (!should_draw_window_border())
        return;

    QPainter painter(this);
    auto color = ChromeStyle::chrome_window_outline(palette());
    auto frame = rect();
    painter.fillRect(QRect(frame.left(), frame.top(), frame.width(), 1), color);
    painter.fillRect(QRect(frame.left(), frame.bottom(), frame.width(), 1), color);
    painter.fillRect(QRect(frame.left(), frame.top(), 1, frame.height()), color);
    painter.fillRect(QRect(frame.right(), frame.top(), 1, frame.height()), color);
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
    if (Application::the().file_downloader().has_active_downloads()) {
        if (visible_browser_window_count() <= 1) {
            if (!Application::the().confirm_cancel_active_downloads(this)) {
                event->ignore();
                return;
            }
        }
    }

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
