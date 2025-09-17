/*
 * Copyright (c) 2022-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Matthew Costa <ucosty@gmail.com>
 * Copyright (c) 2022, Filiph Sandström <filiph.sandstrom@filfatstudios.com>
 * Copyright (c) 2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <LibWebView/Application.h>
#include <UI/Qt/Application.h>
#include <UI/Qt/BrowserWindow.h>
#include <UI/Qt/Icon.h>
#include <UI/Qt/Menu.h>
#include <UI/Qt/Settings.h>
#include <UI/Qt/StringUtils.h>
#include <UI/Qt/TabBar.h>
#include <UI/Qt/WebContentView.h>

#include <QAction>
#include <QActionGroup>
#include <QGuiApplication>
#include <QInputDialog>
#include <QMessageBox>
#include <QMouseEvent>
#include <QScreen>
#include <QShortcut>
#include <QStatusBar>
#include <QWheelEvent>
#include <QWindow>

namespace Ladybird {

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

class HamburgerMenu : public QMenu {
public:
    using QMenu::QMenu;
    virtual ~HamburgerMenu() override = default;

    virtual void showEvent(QShowEvent*) override
    {
        if (!isVisible())
            return;
        auto* browser_window = as<BrowserWindow>(parentWidget());
        if (!browser_window)
            return;
        auto* current_tab = browser_window->current_tab();
        if (!current_tab)
            return;
        // Ensure the hamburger menu placed within the browser window.
        auto* hamburger_button = current_tab->hamburger_button();
        auto button_top_right = hamburger_button->mapToGlobal(hamburger_button->rect().bottomRight());
        move(button_top_right - QPoint(rect().width(), 0));
    }
};

BrowserWindow::BrowserWindow(Vector<URL::URL> const& initial_urls, IsPopupWindow is_popup_window, Tab* parent_tab, Optional<u64> page_index)
    : m_tabs_container(new TabWidget(this))
    , m_new_tab_button_toolbar(new QToolBar("New Tab", m_tabs_container))
    , m_is_popup_window(is_popup_window)
{
    auto const& browser_options = WebView::Application::browser_options();

    setWindowIcon(app_icon());

    // Listen for DPI changes
    m_device_pixel_ratio = devicePixelRatio();
    m_current_screen = screen();
    m_refresh_rate = m_current_screen->refreshRate();

    if (QT_VERSION < QT_VERSION_CHECK(6, 6, 0) || QGuiApplication::platformName() != "wayland") {
        setAttribute(Qt::WA_NativeWindow);
        setAttribute(Qt::WA_DontCreateNativeAncestors);
        QObject::connect(m_current_screen, &QScreen::logicalDotsPerInchChanged, this, &BrowserWindow::device_pixel_ratio_changed);
        QObject::connect(m_current_screen, &QScreen::refreshRateChanged, this, &BrowserWindow::refresh_rate_changed);
        QObject::connect(windowHandle(), &QWindow::screenChanged, this, [this](QScreen* screen) {
            if (m_device_pixel_ratio != devicePixelRatio())
                device_pixel_ratio_changed(devicePixelRatio());

            if (m_refresh_rate != screen->refreshRate())
                refresh_rate_changed(screen->refreshRate());

            // Listen for logicalDotsPerInchChanged and refreshRateChanged signals on new screen
            QObject::disconnect(m_current_screen, &QScreen::logicalDotsPerInchChanged, nullptr, nullptr);
            QObject::disconnect(m_current_screen, &QScreen::refreshRateChanged, nullptr, nullptr);
            m_current_screen = screen;
            QObject::connect(m_current_screen, &QScreen::logicalDotsPerInchChanged, this, &BrowserWindow::device_pixel_ratio_changed);
            QObject::connect(m_current_screen, &QScreen::refreshRateChanged, this, &BrowserWindow::refresh_rate_changed);
        });
    }

    m_hamburger_menu = new HamburgerMenu(this);

    if (!Settings::the()->show_menubar())
        menuBar()->hide();

    QObject::connect(Settings::the(), &Settings::show_menubar_changed, this, [this](bool show_menubar) {
        menuBar()->setVisible(show_menubar);
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

    auto* close_current_tab_action = new QAction("&Close Current Tab", this);
    close_current_tab_action->setIcon(load_icon_from_uri("resource://icons/16x16/close-tab.png"sv));
    close_current_tab_action->setShortcuts(QKeySequence::keyBindings(QKeySequence::StandardKey::Close));
    m_hamburger_menu->addAction(close_current_tab_action);
    file_menu->addAction(close_current_tab_action);

    auto* open_file_action = new QAction("&Open File...", this);
    open_file_action->setIcon(load_icon_from_uri("resource://icons/16x16/filetype-folder-open.png"sv));
    open_file_action->setShortcut(QKeySequence(QKeySequence::StandardKey::Open));
    m_hamburger_menu->addAction(open_file_action);
    file_menu->addAction(open_file_action);

    m_hamburger_menu->addSeparator();

    auto* edit_menu = m_hamburger_menu->addMenu("&Edit");
    menuBar()->addMenu(edit_menu);

    edit_menu->addAction(create_application_action(*this, Application::the().copy_selection_action()));
    edit_menu->addAction(create_application_action(*this, Application::the().paste_action()));
    edit_menu->addAction(create_application_action(*this, Application::the().select_all_action()));
    edit_menu->addSeparator();

    m_find_in_page_action = new QAction("&Find in Page...", this);
    m_find_in_page_action->setIcon(load_icon_from_uri("resource://icons/16x16/find.png"sv));
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
    edit_menu->addAction(create_application_action(*edit_menu, Application::the().open_settings_page_action()));

    auto* view_menu = m_hamburger_menu->addMenu("&View");
    menuBar()->addMenu(view_menu);

    auto* open_next_tab_action = new QAction("Open &Next Tab", this);
    open_next_tab_action->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_PageDown));
    view_menu->addAction(open_next_tab_action);
    QObject::connect(open_next_tab_action, &QAction::triggered, this, &BrowserWindow::open_next_tab);

    auto* open_previous_tab_action = new QAction("Open &Previous Tab", this);
    open_previous_tab_action->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_PageUp));
    view_menu->addAction(open_previous_tab_action);
    QObject::connect(open_previous_tab_action, &QAction::triggered, this, &BrowserWindow::open_previous_tab);

    view_menu->addSeparator();

    view_menu->addMenu(create_application_menu(*view_menu, Application::the().zoom_menu()));
    view_menu->addSeparator();

    view_menu->addMenu(create_application_menu(*view_menu, Application::the().color_scheme_menu()));
    view_menu->addMenu(create_application_menu(*view_menu, Application::the().contrast_menu()));
    view_menu->addMenu(create_application_menu(*view_menu, Application::the().motion_menu()));
    view_menu->addSeparator();

    auto* show_menubar = new QAction("Show &Menubar", this);
    show_menubar->setCheckable(true);
    show_menubar->setChecked(Settings::the()->show_menubar());
    view_menu->addAction(show_menubar);
    QObject::connect(show_menubar, &QAction::triggered, this, [](bool checked) {
        Settings::the()->set_show_menubar(checked);
    });

    auto* inspect_menu = create_application_menu(*m_hamburger_menu, Application::the().inspect_menu());
    m_hamburger_menu->addMenu(inspect_menu);
    menuBar()->addMenu(inspect_menu);

    auto* debug_menu = create_application_menu(*m_hamburger_menu, Application::the().debug_menu());
    m_hamburger_menu->addMenu(debug_menu);
    menuBar()->addMenu(debug_menu);

    auto* help_menu = m_hamburger_menu->addMenu("&Help");
    menuBar()->addMenu(help_menu);

    help_menu->addAction(create_application_action(*help_menu, Application::the().open_about_page_action()));

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
        (void)Application::the().new_window({});
    });
    QObject::connect(open_file_action, &QAction::triggered, this, &BrowserWindow::open_file);
    QObject::connect(m_tabs_container, &QTabWidget::currentChanged, [this](int index) {
        auto* tab = as<Tab>(m_tabs_container->widget(index));
        if (tab)
            setWindowTitle(QString("%1 - Ladybird").arg(tab->title()));

        set_current_tab(tab);
    });
    QObject::connect(m_tabs_container, &QTabWidget::tabCloseRequested, this, &BrowserWindow::close_tab);
    QObject::connect(close_current_tab_action, &QAction::triggered, this, &BrowserWindow::close_current_tab);

    for (int i = 0; i <= 7; ++i) {
        new QShortcut(QKeySequence(Qt::CTRL | static_cast<Qt::Key>(Qt::Key_1 + i)), this, [this, i] {
            if (m_tabs_container->count() <= 1)
                return;

            m_tabs_container->setCurrentIndex(min(i, m_tabs_container->count() - 1));
        });
    }

    new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_9), this, [this] {
        if (m_tabs_container->count() <= 1)
            return;

        m_tabs_container->setCurrentIndex(m_tabs_container->count() - 1);
    });

    if (parent_tab) {
        new_child_tab(Web::HTML::ActivateTab::Yes, *parent_tab, AK::move(page_index));
    } else {
        for (size_t i = 0; i < initial_urls.size(); ++i) {
            new_tab_from_url(initial_urls[i], (i == 0) ? Web::HTML::ActivateTab::Yes : Web::HTML::ActivateTab::No);
        }
    }

    m_new_tab_button_toolbar->addAction(m_new_tab_action);
    m_new_tab_button_toolbar->setMovable(false);
    m_new_tab_button_toolbar->setStyleSheet("QToolBar { background: transparent; }");
    m_new_tab_button_toolbar->setIconSize(QSize(16, 16));
    m_tabs_container->setCornerWidget(m_new_tab_button_toolbar, Qt::TopRightCorner);

    setCentralWidget(m_tabs_container);
    setContextMenuPolicy(Qt::PreventContextMenu);

    if (browser_options.devtools_port.has_value())
        on_devtools_enabled();
}

void BrowserWindow::on_devtools_enabled()
{
    auto* disable_button = new QPushButton("Disable", this);

    connect(disable_button, &QPushButton::clicked, this, []() {
        MUST(WebView::Application::the().toggle_devtools_enabled());
    });

    statusBar()->addPermanentWidget(disable_button);

    auto message = MUST(String::formatted("DevTools is enabled on port {}", WebView::Application::browser_options().devtools_port));
    statusBar()->showMessage(qstring_from_ak_string(message));
}

void BrowserWindow::on_devtools_disabled()
{
    setStatusBar(nullptr);
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

    m_tabs_container->addTab(tab, "New Tab");
    if (activate_tab == Web::HTML::ActivateTab::Yes)
        m_tabs_container->setCurrentWidget(tab);

    initialize_tab(tab);
    return *tab;
}

Tab& BrowserWindow::create_new_tab(Web::HTML::ActivateTab activate_tab)
{
    auto* tab = new Tab(this);

    if (m_current_tab == nullptr) {
        set_current_tab(tab);
    }

    m_tabs_container->addTab(tab, "New Tab");
    if (activate_tab == Web::HTML::ActivateTab::Yes)
        m_tabs_container->setCurrentWidget(tab);

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
            auto& window = Application::the().new_window({}, IsPopupWindow::Yes, tab, AK::move(page_index));
            window.set_window_rect(hints.screen_x, hints.screen_y, hints.width, hints.height);
            return window.current_tab()->view().handle();
        }
        auto& new_tab = new_child_tab(activate_tab, *tab, page_index);
        return new_tab.view().handle();
    };

    m_tabs_container->setTabIcon(m_tabs_container->indexOf(tab), tab->favicon());
    create_close_button_for_tab(tab);
}

void BrowserWindow::activate_tab(int index)
{
    m_tabs_container->setCurrentIndex(index);
}

void BrowserWindow::close_tab(int index)
{
    auto* tab = m_tabs_container->widget(index);
    m_tabs_container->removeTab(index);
    tab->deleteLater();

    if (m_tabs_container->count() == 0)
        close();
}

void BrowserWindow::move_tab(int old_index, int new_index)
{
    m_tabs_container->tabBar()->moveTab(old_index, new_index);
}

void BrowserWindow::open_file()
{
    m_current_tab->open_file();
}

void BrowserWindow::close_current_tab()
{
    close_tab(m_tabs_container->currentIndex());
}

int BrowserWindow::tab_index(Tab* tab)
{
    return m_tabs_container->indexOf(tab);
}

void BrowserWindow::device_pixel_ratio_changed(qreal dpi)
{
    m_device_pixel_ratio = dpi;
    for_each_tab([this](auto& tab) {
        tab.view().set_device_pixel_ratio(m_device_pixel_ratio);
    });
}

void BrowserWindow::refresh_rate_changed(qreal refresh_rate)
{
    m_refresh_rate = refresh_rate;
    for_each_tab([this](auto& tab) {
        tab.view().set_maximum_frames_per_second(m_refresh_rate);
    });
}

void BrowserWindow::tab_title_changed(int index, QString const& title)
{
    // NOTE: Qt uses ampersands for shortcut keys in tab titles, so we need to escape them.
    QString title_escaped = title;
    title_escaped.replace("&", "&&");

    m_tabs_container->setTabText(index, title_escaped);
    m_tabs_container->setTabToolTip(index, title);

    if (m_tabs_container->currentIndex() == index)
        setWindowTitle(QString("%1 - Ladybird").arg(title));
}

void BrowserWindow::tab_favicon_changed(int index, QIcon const& icon)
{
    m_tabs_container->setTabIcon(index, icon);
}

void BrowserWindow::create_close_button_for_tab(Tab* tab)
{
    auto index = m_tabs_container->indexOf(tab);
    m_tabs_container->setTabIcon(index, tab->favicon());

    auto* button = new TabBarButton(create_tvg_icon_with_theme_colors("close", palette()));
    auto position = audio_button_position_for_tab(index) == QTabBar::LeftSide ? QTabBar::RightSide : QTabBar::LeftSide;

    connect(button, &QPushButton::clicked, this, [this, tab]() {
        auto index = m_tabs_container->indexOf(tab);
        close_tab(index);
    });

    m_tabs_container->tabBar()->setTabButton(index, position, button);
}

void BrowserWindow::tab_audio_play_state_changed(int index, Web::HTML::AudioPlayState play_state)
{
    auto* tab = as<Tab>(m_tabs_container->widget(index));
    auto position = audio_button_position_for_tab(index);

    switch (play_state) {
    case Web::HTML::AudioPlayState::Paused:
        if (tab->view().page_mute_state() == Web::HTML::MuteState::Unmuted)
            m_tabs_container->tabBar()->setTabButton(index, position, nullptr);
        break;

    case Web::HTML::AudioPlayState::Playing:
        auto* button = new TabBarButton(icon_for_page_mute_state(*tab));
        button->setToolTip(tool_tip_for_page_mute_state(*tab));
        button->setObjectName("LadybirdAudioState");

        connect(button, &QPushButton::clicked, this, [this, tab, position]() {
            tab->view().toggle_page_mute_state();
            auto index = tab_index(tab);

            switch (tab->view().audio_play_state()) {
            case Web::HTML::AudioPlayState::Paused:
                m_tabs_container->tabBar()->setTabButton(index, position, nullptr);
                break;
            case Web::HTML::AudioPlayState::Playing:
                auto* button = m_tabs_container->tabBar()->tabButton(index, position);
                as<TabBarButton>(button)->setIcon(icon_for_page_mute_state(*tab));
                button->setToolTip(tool_tip_for_page_mute_state(*tab));
                break;
            }
        });

        m_tabs_container->tabBar()->setTabButton(index, position, button);
        break;
    }
}

QIcon BrowserWindow::icon_for_page_mute_state(Tab& tab) const
{
    switch (tab.view().page_mute_state()) {
    case Web::HTML::MuteState::Muted:
        return style()->standardIcon(QStyle::SP_MediaVolumeMuted);
    case Web::HTML::MuteState::Unmuted:
        return style()->standardIcon(QStyle::SP_MediaVolume);
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

QTabBar::ButtonPosition BrowserWindow::audio_button_position_for_tab(int tab_index) const
{
    if (auto* button = m_tabs_container->tabBar()->tabButton(tab_index, QTabBar::LeftSide)) {
        if (button->objectName() != "LadybirdAudioState")
            return QTabBar::RightSide;
    }

    return QTabBar::LeftSide;
}

void BrowserWindow::open_next_tab()
{
    if (m_tabs_container->count() <= 1)
        return;

    auto next_index = m_tabs_container->currentIndex() + 1;
    if (next_index >= m_tabs_container->count())
        next_index = 0;
    m_tabs_container->setCurrentIndex(next_index);
}

void BrowserWindow::open_previous_tab()
{
    if (m_tabs_container->count() <= 1)
        return;

    auto next_index = m_tabs_container->currentIndex() - 1;
    if (next_index < 0)
        next_index = m_tabs_container->count() - 1;
    m_tabs_container->setCurrentIndex(next_index);
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

bool BrowserWindow::event(QEvent* event)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    if (event->type() == QEvent::DevicePixelRatioChange) {
        if (m_device_pixel_ratio != devicePixelRatio())
            device_pixel_ratio_changed(devicePixelRatio());
    }
#endif

    if (event->type() == QEvent::WindowActivate)
        Application::the().set_active_window(*this);

    return QMainWindow::event(event);
}

void BrowserWindow::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);

    for_each_tab([&](auto& tab) {
        tab.view().set_window_size({ width(), height() });
    });
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

bool BrowserWindow::eventFilter(QObject* obj, QEvent* event)
{
    if (event->type() == QEvent::MouseButtonRelease) {
        auto const* const mouse_event = static_cast<QMouseEvent*>(event);
        if (mouse_event->button() == Qt::MouseButton::MiddleButton) {
            if (obj == m_tabs_container) {
                auto const tab_index = m_tabs_container->tabBar()->tabAt(mouse_event->pos());
                if (tab_index != -1) {
                    close_tab(tab_index);
                    return true;
                }
            }
        }
    }

    return QMainWindow::eventFilter(obj, event);
}

void BrowserWindow::closeEvent(QCloseEvent* event)
{
    if (m_is_popup_window == IsPopupWindow::No) {
        Settings::the()->set_last_position(pos());
        Settings::the()->set_last_size(size());
        Settings::the()->set_is_maximized(isMaximized());
    }

    QObject::deleteLater();

    QMainWindow::closeEvent(event);
}

}
