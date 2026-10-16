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

#include <AK/Platform.h>
#include <AK/StdLibExtras.h>
#include <AK/TypeCasts.h>
#include <LibWebView/Application.h>
#include <LibWebView/HistoryStore.h>
#include <LibWebView/SessionStore.h>
#include <LibWebView/URL.h>
#include <UI/Qt/Application.h>
#include <UI/Qt/BrowserWindow.h>
#include <UI/Qt/ChromeLayout.h>
#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/DevToolsBanner.h>
#include <UI/Qt/Icon.h>
#include <UI/Qt/Menu.h>
#include <UI/Qt/StringUtils.h>
#if defined(AK_OS_MACOS)
#    include <UI/Qt/MacWindow.h>
#endif
#include <UI/Qt/TabBar.h>
#include <UI/Qt/WebContentView.h>
#include <UI/Qt/WindowControlButton.h>

#include <QAbstractButton>
#include <QApplication>
#include <QContextMenuEvent>
#include <QCursor>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QMenuBar>
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
#include <QWidgetAction>
#include <QWindow>

namespace Ladybird {

static constexpr auto AUDIO_STATE_BUTTON_POSITION = QTabBar::LeftSide;
static constexpr auto TAB_CLOSE_BUTTON_POSITION = QTabBar::RightSide;
static constexpr auto WINDOW_DRAG_REGION_PROPERTY = "LadybirdWindowDragRegion";
static constexpr int WINDOW_RESIZE_BORDER_WIDTH = 6;
static constexpr int WINDOW_RESIZE_CORNER_WIDTH = WINDOW_RESIZE_BORDER_WIDTH * 2;
#if defined(AK_OS_MACOS)
static constexpr int NATIVE_WINDOW_CONTROL_X_OFFSET = 6;
static constexpr int NATIVE_WINDOW_CONTROL_Y_OFFSET = 4;
#endif

static bool is_empty_window_drag_region(QWidget const& widget, QPoint position)
{
    return widget.property(WINDOW_DRAG_REGION_PROPERTY).toBool() && widget.childAt(position) == nullptr;
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
    // Let button float in place for 3 times the time it takes to animate it in place.
    m_debounce.start(*this, button_animation_time() * 3);
    m_exit_button->animate_show();
}

bool FullscreenMode::is_api_fullscreen() const
{
    return m_fullscreen_tab;
}

void FullscreenMode::maybe_animate_show_exit_button(QPointF pos)
{
    u64 const mouse_y = static_cast<u64>(pos.y());
    u64 const threshold = static_cast<u64>(m_window->height() * 0.01);

    if (m_debounce.is_active()) {
        return;
    }

    // Display the button if the mouse is 1% from the top
    if (mouse_y <= threshold) {
        if (!m_exit_button->isVisible()) {
            m_debounce.start(*this, button_animation_time() * 3);
            m_exit_button->animate_show();
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

BrowserWindow::BrowserWindow(Vector<URL::URL> const& initial_urls, IsPopupWindow is_popup_window, WebView::IsPrivate is_private, Tab* parent_tab, Optional<WebView::CanonicalTraversable&> traversable, Optional<WebView::SessionWindowId> restored_session_window_id)
    : m_is_private(is_private)
    , m_session(WebView::Application::session_for_new_view(is_private))
    , m_tabs_container(new TabWidget(this))
    , m_is_popup_window(is_popup_window)
{
    auto const& browser_options = WebView::Application::browser_options();

    // A restored window keeps the session row the store re-adopted for it.
    if (restored_session_window_id.has_value())
        m_session_window_id = restored_session_window_id;
    else
        register_window_with_session_store();

#if defined(AK_OS_MACOS)
    setWindowFlag(Qt::ExpandedClientAreaHint);
    setWindowFlag(Qt::NoTitleBarBackgroundHint);
    setAttribute(Qt::WA_ContentsMarginsRespectsSafeArea, false);
#else
    setWindowFlag(Qt::FramelessWindowHint, uses_client_side_decorations());
#endif
    setAttribute(Qt::WA_OpaquePaintEvent);
    setWindowIcon(app_icon());
    qApp->installEventFilter(this);
    update_window_border();

    update_tabs_display();

    m_screen_observer = new WindowScreenObserver(*this);
    m_screen_observer->on_device_pixel_ratio_change = [this] {
        for_each_tab([this](auto& tab) {
            tab.view().set_device_pixel_ratio(m_screen_observer->device_pixel_ratio());
        });
    };
    m_screen_observer->on_display_metadata_change = [this] {
        for_each_tab([this](auto& tab) {
            tab.view().set_display_metadata(m_screen_observer->display_id(), m_screen_observer->refresh_rate());
        });
    };

    initialize_application_actions();
    initialize_application_menu();
    initialize_hamburger_menu();
    initialize_window_context_menu();
    update_chrome_style();

    m_exit_button = new ExitFullscreenButton { this };
    m_fullscreen_mode = new FullscreenMode { this, m_exit_button };
    connect(m_fullscreen_mode, &FullscreenMode::on_exit_fullscreen, this, &BrowserWindow::exit_fullscreen);
    connect(m_fullscreen_mode, &FullscreenMode::on_exit_fullscreen, m_exit_button, &ExitFullscreenButton::hide);

    QObject::connect(m_tabs_container, &TabWidget::current_tab_changed, this, [this](int index) {
        auto* tab = m_tabs_container->tab(index);
        if (tab)
            update_window_title(tab->title());

        set_current_tab(tab);
        if (tab) {
            QWidget* focus_widget = &tab->view();
            if (auto* location_edit = tab->location_edit_with_parked_editing())
                focus_widget = location_edit;
            else if (auto* tab_focus_widget = tab->focusWidget(); tab_focus_widget && tab->isAncestorOf(tab_focus_widget))
                focus_widget = tab_focus_widget;
#if defined(AK_OS_MACOS)
            make_appkit_window_first_responder(*focus_widget);
#endif
            focus_widget->setFocus();
        }
        fullscreen_mode().exit(FullscreenMode::ExitInitiatedBy::UI);
    });
    QObject::connect(m_tabs_container, &TabWidget::tab_close_requested, this, &BrowserWindow::request_to_close_tab);
    QObject::connect(m_tabs_container->tab_bar(), &QTabBar::tabMoved, this, [this](int, int) {
        sync_session_tab_order();
    });

    if (parent_tab) {
        create_new_tab(Web::HTML::ActivateTab::Yes, traversable);
    } else {
        for (size_t i = 0; i < initial_urls.size(); ++i)
            create_new_tab((i == 0) ? Web::HTML::ActivateTab::Yes : Web::HTML::ActivateTab::No, TabLocation::end());
    }

    m_tabs_container->set_new_tab_action(Application::the().new_tab_action());

    auto* main_widget = new QWidget(this);
    auto* main_layout = new QVBoxLayout(main_widget);
    main_layout->setContentsMargins(0, 0, 0, 0);
    main_layout->setSpacing(0);
    main_layout->addWidget(m_tabs_container, 1);

    m_devtools_banner = new DevToolsBanner(main_widget);
    connect(m_devtools_banner, &DevToolsBanner::launch_client_requested, this, [] {
        if (auto result = WebView::Application::the().launch_devtools_client(); result.is_error())
            WebView::Application::the().display_error_dialog(MUST(String::formatted("Unable to launch the DevTools client: {}", result.error())));
    });
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

void BrowserWindow::initialize_application_actions()
{
    auto& application = Application::the();

    // Add all actions to the window itself. This allows shortcuts to activate when the system menu bar is not visible.
    addAction(application.new_tab_action());
    addAction(application.new_window_action());
    addAction(application.new_private_window_action());
    addAction(application.reopen_recently_closed_tab_action());
    addAction(application.close_current_tab_action());
    addAction(application.open_next_tab_action());
    addAction(application.open_previous_tab_action());
    addAction(application.open_file_action());
    addAction(application.open_settings_action());
    addAction(application.open_downloads_action());
    addAction(application.toggle_bookmark_bar_action());
    addAction(application.find_in_page_action());
    addAction(application.zoom_in_action());
    addAction(application.zoom_out_action());
    addAction(application.reset_zoom_action());
    addAction(application.quit_action());

    for (auto const& shortcut : QKeySequence::keyBindings(QKeySequence::StandardKey::FindPrevious)) {
        new QShortcut(shortcut, this, [this]() {
            if (m_current_tab)
                m_current_tab->find_previous();
        });
    }

    for (auto const& shortcut : QKeySequence::keyBindings(QKeySequence::StandardKey::FindNext)) {
        new QShortcut(shortcut, this, [this]() {
            if (m_current_tab)
                m_current_tab->find_next();
        });
    }

    for (int i = 0; i < 8; ++i) {
        new QShortcut(QKeySequence(Qt::CTRL | static_cast<Qt::Key>(Qt::Key_1 + i)), this, [this, i]() {
            if (m_tabs_container->count() > 1)
                m_tabs_container->set_current_index(min(i, m_tabs_container->count() - 1));
        });
    }

    new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_9), this, [this]() {
        if (m_tabs_container->count() > 1)
            m_tabs_container->set_current_index(m_tabs_container->count() - 1);
    });
}

void BrowserWindow::initialize_application_menu()
{
    setMenuBar(Application::the().create_application_menu_bar(Application::ForBrowserWindow::Yes));
    menuBar()->setObjectName("LadybirdMenuBar");

    create_menu_bar_window_controls();
    update_menu_bar_visibility();
}

void BrowserWindow::initialize_hamburger_menu()
{
    auto& application = Application::the();

    m_hamburger_menu = new QMenu(this);
    QObject::connect(m_hamburger_menu, &QMenu::aboutToShow, this, &BrowserWindow::update_hamburger_zoom_label);

    m_hamburger_menu->addAction(application.new_tab_action());
    m_hamburger_menu->addAction(application.new_window_action());
    m_hamburger_menu->addAction(application.new_private_window_action());
    m_hamburger_menu->addAction(application.reopen_recently_closed_tab_action());
    m_hamburger_menu->addSeparator();

    m_hamburger_menu->addMenu(application.history_menu());
    m_hamburger_menu->addMenu(application.bookmarks_menu());
    m_hamburger_menu->addAction(application.open_downloads_action());
    m_hamburger_menu->addSeparator();

    m_hamburger_menu->addAction(create_hamburger_zoom_actions());
    m_hamburger_menu->addSeparator();

    m_hamburger_menu->addAction(application.find_in_page_action());
    m_hamburger_menu->addSeparator();

    m_hamburger_menu->addAction(application.open_settings_action());
    m_hamburger_menu->addMenu(application.inspect_menu());
    m_hamburger_menu->addMenu(application.debug_menu());
    m_hamburger_menu->addSeparator();

    m_hamburger_menu->addMenu(application.help_menu());
    m_hamburger_menu->addAction(application.quit_action());
}

QAction* BrowserWindow::create_hamburger_zoom_actions()
{
    auto& application = Application::the();

    auto* container = new QWidget(m_hamburger_menu);
    container->setObjectName("LadybirdHamburgerZoomActions");

    auto* layout = new QHBoxLayout(container);
    layout->setContentsMargins(14, 2, 14, 2);
    layout->setSpacing(4);
    container->setLayout(layout);

    auto* label = new QLabel("Zoom", container);
    layout->addWidget(label);

    auto make_button = [&](QAnyStringView name, QString const& text, QString const& tooltip, QAction* action, int width = 24) {
        auto* button = new QPushButton(text, container);
        button->setObjectName(name);
        button->setToolTip(tooltip);
        button->setFixedSize(width, 24);
        button->setFlat(true);

        QObject::connect(button, &QPushButton::clicked, action, &QAction::trigger);
        QObject::connect(action, &QAction::triggered, this, &BrowserWindow::update_hamburger_zoom_label);

        layout->addWidget(button);
        return button;
    };

    make_button("LadybirdHamburgerZoomOutButton", "-", "Zoom out", application.zoom_out_action());
    m_zoom_level = make_button("LadybirdHamburgerResetZoomButton", "100%", "Reset zoom level", application.reset_zoom_action(), 50);
    make_button("LadybirdHamburgerZoomInButton", "+", "Zoom in", application.zoom_in_action());

    auto* action = new QWidgetAction(m_hamburger_menu);
    action->setDefaultWidget(container);
    return action;
}

void BrowserWindow::initialize_window_context_menu()
{
    m_window_context_menu = new QMenu(this);

    auto* new_tab_action = m_window_context_menu->addAction("New &Tab");
    QObject::connect(new_tab_action, &QAction::triggered, this, &BrowserWindow::open_new_tab);

    m_reopen_closed_tab_action = m_window_context_menu->addAction("&Reopen Closed Tab");
    QObject::connect(m_reopen_closed_tab_action, &QAction::triggered, this, [this] {
        Application::the().reopen_recently_closed_tab(this);
    });

    m_window_context_menu->addSeparator();

    m_always_on_top_action = m_window_context_menu->addAction("Always on T&op");
    m_always_on_top_action->setCheckable(true);
    QObject::connect(m_always_on_top_action, &QAction::triggered, this, &BrowserWindow::set_always_on_top);

    m_window_context_menu->addSeparator();

    m_toggle_vertical_tabs_action = m_window_context_menu->addAction(QString {});
    QObject::connect(m_toggle_vertical_tabs_action, &QAction::triggered, this, [] {
        auto tab_settings = Application::settings().tab_settings();
        tab_settings.vertical_tabs_enabled = !tab_settings.vertical_tabs_enabled;
        Application::settings().set_tab_settings(tab_settings);
    });
}

void BrowserWindow::show_window_context_menu(QPoint global_position)
{
    auto vertical_tabs_enabled = Application::settings().tab_settings().vertical_tabs_enabled;

    m_reopen_closed_tab_action->setEnabled(m_session->session_store->has_closed_units());
    m_always_on_top_action->setChecked(windowFlags().testFlag(Qt::WindowStaysOnTopHint));
    m_toggle_vertical_tabs_action->setText(vertical_tabs_enabled ? "Switch to &Horizontal Tabs" : "Switch to &Vertical Tabs");

    execute_context_menu(*m_window_context_menu, global_position);
}

void BrowserWindow::update_hamburger_zoom_label()
{
    auto zoom_level = round_to<int>(m_current_tab->view().zoom_level() * 100);
    m_zoom_level->setText(qformatted("{}%", zoom_level));
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
    for_each_tab([](Tab& tab) {
        tab.bookmarks_bar().rebuild();
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

Tab& BrowserWindow::new_tab_from_url(URL::URL const& url, Web::HTML::ActivateTab activate_tab, TabLocation location)
{
    auto& tab = create_new_tab(activate_tab, location);
    tab.navigate(url);
    return tab;
}

void BrowserWindow::open_new_tab()
{
    auto& tab = new_tab_from_url(WebView::Application::settings().new_tab_page_url(), Web::HTML::ActivateTab::Yes, TabLocation::end());
    tab.set_url_is_hidden(true);
    tab.focus_location_editor();
}

void BrowserWindow::duplicate_tab(Tab& source_tab)
{
    // A tab that hasn't loaded keeps its history in the session store.
    Optional<WebView::SessionHistorySnapshot> history;
    if (source_tab.is_loaded())
        history = source_tab.view().session_history_snapshot();
    else if (auto session_tab_id = source_tab.session_tab_id(); session_tab_id.has_value())
        history = m_session->session_store->load_tab_history(*session_tab_id);
    auto source_url = source_tab.url();

    auto& duplicate = create_new_tab(Web::HTML::ActivateTab::Yes, TabLocation::after_tab(source_tab));
    if (!history.has_value() || duplicate.view().restore_session_history_from_snapshot(history.release_value()).is_error())
        duplicate.navigate(source_url);
}

void BrowserWindow::open_history_entry_in_new_tab(Tab& source_tab, int delta)
{
    auto& view = source_tab.view();
    auto history = view.session_history_snapshot();
    history.current_used_step_index = view.traversable().effective_current_session_history_step_index() + delta;
    if (history.current_used_step_index >= history.used_steps.size())
        return;

    auto& tab = create_new_tab(Web::HTML::ActivateTab::No, TabLocation::after_tab(source_tab));
    (void)tab.view().restore_session_history_from_snapshot(AK::move(history));
}

void BrowserWindow::restore_tabs(Vector<WebView::RestoredSessionTab> const& tabs, i64 active_tab_index)
{
    if (tabs.is_empty())
        return;

    // The first tab a strip gets becomes its current tab, so the active tab goes in first, at what's then index 0, and
    // every other tab goes in around it at its own index.
    auto active_index = static_cast<size_t>(clamp<i64>(active_tab_index, 0, static_cast<i64>(tabs.size()) - 1));
    create_restored_tab(tabs[active_index], 0, false);
    for (size_t i = 0; i < tabs.size(); ++i) {
        if (i != active_index)
            create_restored_tab(tabs[i], static_cast<int>(i), !tabs[i].pinned);
    }
}

Tab& BrowserWindow::create_restored_tab(WebView::RestoredSessionTab const& restored, int index, bool defer)
{
    if (defer) {
        // Until the tab loads, it shows what browsing history knows about its page.
        auto entry = m_session->history_store->entry_for_url(restored.url);
        DeferredTab deferred_tab {
            .session_tab_id = restored.tab_id,
            .url = restored.url,
            .title = entry.has_value() && entry->title.has_value() ? qstring_from_ak_string(*entry->title) : qstring_from_ak_string(restored.url.serialize()),
            .favicon = entry.has_value() && entry->favicon_png.has_value() ? icon_from_png(entry->favicon_png->bytes(), TAB_ICON_SIZE) : QIcon {},
        };
        auto* tab = new Tab(this, AK::move(deferred_tab));
        tab->set_pinned(restored.pinned);
        m_tabs_container->insert_tab(index, tab, tab->title());
        initialize_tab(tab);
        tab_title_changed(m_tabs_container->index_of(tab), tab->title());
        return *tab;
    }

    auto* tab = new Tab(this);
    // The tab already has a row, so initialize_tab() doesn't register a new one.
    tab->set_session_tab_id(restored.tab_id);
    tab->set_pinned(restored.pinned);

    if (!m_current_tab)
        set_current_tab(tab);
    m_tabs_container->insert_tab(index, tab, "New Tab");
    initialize_tab(tab);

    auto history = m_session->session_store->load_tab_history(restored.tab_id);
    if (!history.has_value() || tab->view().restore_session_history_from_snapshot(history.release_value()).is_error())
        tab->navigate(restored.url);
    return *tab;
}

Tab& BrowserWindow::create_new_tab(Web::HTML::ActivateTab activate_tab, Optional<WebView::CanonicalTraversable&> traversable)
{
    cancel_window_close_preflight();
    if (!traversable.has_value())
        return create_new_tab(activate_tab, TabLocation::end());

    auto* tab = new Tab(this, traversable);

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
    return !use_native_macos_window_controls() && WebView::Application::settings().config_variable_as_bool(WebView::ConfigVariableID::UseClientSideWindowDecorations);
}

bool BrowserWindow::has_chrome_in_titlebar()
{
    return use_native_macos_window_controls() || uses_client_side_decorations();
}

Tab& BrowserWindow::create_new_tab(Web::HTML::ActivateTab activate_tab, TabLocation location)
{
    cancel_window_close_preflight();

    auto* tab = new Tab(this);

    if (m_current_tab == nullptr) {
        set_current_tab(tab);
    }

    auto insertion_index = m_tabs_container->count();
    switch (location.kind()) {
    case TabLocation::Kind::End:
        break;
    case TabLocation::Kind::AfterCurrentTab:
        insertion_index = m_tabs_container->current_index() + 1;
        break;
    case TabLocation::Kind::AfterTab:
        if (auto* source_tab = location.tab(); source_tab) {
            if (auto source_tab_index = tab_index(source_tab); source_tab_index != -1)
                insertion_index = source_tab_index + 1;
        }
        break;
    case TabLocation::Kind::AtIndex:
        insertion_index = location.index();
        break;
    }
    // A new tab is unpinned, so it never lands inside the pinned group.
    m_tabs_container->insert_tab(constrain_insertion_index(insertion_index, false), tab, "New Tab");

    if (activate_tab == Web::HTML::ActivateTab::Yes)
        m_tabs_container->set_current_tab(tab);

    initialize_tab(tab);

    return *tab;
}

void BrowserWindow::initialize_tab(Tab* tab)
{
    if (m_session_window_id.has_value() && !tab->session_tab_id().has_value()) {
        auto insertion_index = tab_index(tab);
        WebView::SessionStore::TabOpened opened {
            .window_id = m_session_window_id,
            .initial_url = {},
            .insertion_index = insertion_index >= 0 ? Optional<i64> { insertion_index } : Optional<i64> {},
            .is_active = m_current_tab == tab ? WebView::SessionStore::IsActive::Yes : WebView::SessionStore::IsActive::No,
        };
        auto session_tab_id = m_session->session_store->tab_opened(AK::move(opened));
        if (session_tab_id.is_error())
            dbgln("Unable to register the new tab with the session store: {}", session_tab_id.error());
        else
            tab->set_session_tab_id(session_tab_id.value());
    }

    QObject::connect(tab, &Tab::title_changed, this, &BrowserWindow::tab_title_changed);
    QObject::connect(tab, &Tab::favicon_changed, this, &BrowserWindow::tab_favicon_changed);
    QObject::connect(tab, &Tab::audio_play_state_changed, this, &BrowserWindow::tab_audio_play_state_changed);
    QObject::connect(tab, &Tab::loading_changed, this, [this] {
        m_tabs_container->tab_bar()->update_loading_swirl_animation();
    });
    QObject::connect(tab, &Tab::loading_finished, this, [this, tab] {
        m_tabs_container->tab_bar()->play_load_sheen(*tab);
    });

    if (tab->is_loaded())
        initialize_tab_view(tab);
    else
        QObject::connect(tab, &Tab::loaded, this, [this, tab] { initialize_tab_view(tab); }, Qt::SingleShotConnection);

    initialize_tab_buttons(tab);
}

void BrowserWindow::initialize_tab_view(Tab* tab)
{
    QObject::connect(&tab->view(), &WebContentView::urls_dropped, this, [this](auto& urls) {
        VERIFY(urls.size());
        m_current_tab->view().load_from_user_input(ak_url_from_qurl(urls[0]));

        for (qsizetype i = 1; i < urls.size(); ++i) {
            auto url = ak_url_from_qurl(urls[i]);
            if (WebView::is_url_handled_internally(url))
                new_tab_from_url(url, Web::HTML::ActivateTab::No, TabLocation::end());
            else
                m_current_tab->view().open_url_in_new_tab(url, Web::HTML::ActivateTab::No);
        }
    });

    tab->view().on_new_web_view = [this, tab](auto activate_tab, Web::HTML::WebViewHints hints, WebView::CanonicalTraversable& traversable) {
        if (hints.popup) {
            auto cascaded_configuration = Application::the().configuration_for_new_window();
            WindowConfiguration configuration {
                .x = hints.screen_x.has_value() ? hints.screen_x : cascaded_configuration.x,
                .y = hints.screen_y.has_value() ? hints.screen_y : cascaded_configuration.y,
                .width = hints.width,
                .height = hints.height,
            };
            auto& window = Application::the().new_window({}, configuration, IsPopupWindow::Yes, m_is_private, tab, traversable);
            return window.current_tab()->view().handle();
        }
        auto& new_tab = create_new_tab(activate_tab, traversable);
        return new_tab.view().handle();
    };
}

void BrowserWindow::uninitialize_tab(Tab* tab)
{
    QObject::disconnect(tab, nullptr, this, nullptr);
    if (tab->is_loaded())
        QObject::disconnect(&tab->view(), nullptr, this, nullptr);
}

void BrowserWindow::adopt_tab(Tab& tab, int index)
{
    cancel_window_close_preflight();

    // A tab keeps its pin in its new window, inside that window's matching group.
    index = constrain_insertion_index(index, tab.is_pinned());

    tab.set_window(*this);
    m_tabs_container->insert_tab(index, &tab, "New Tab");
    if (tab.is_loaded())
        tab.view().finish_window_move();
    if (!m_session_window_id.has_value())
        register_window_with_session_store();
    auto moved_tab_id = tab.session_tab_id();
    initialize_tab(&tab);
    tab_title_changed(index, tab.title());

    if (moved_tab_id.has_value()) {
        if (m_session_window_id.has_value()) {
            WebView::SessionStore::TabMoved moved {
                .tab_id = *moved_tab_id,
                .new_window_id = *m_session_window_id,
                .ordinal = index,
            };
            m_session->session_store->tab_moved(AK::move(moved));
            sync_session_tab_order();
        } else {
            // The source window's reported order no longer includes this tab; it must leave
            // tracking rather than stay mapped there.
            m_session->session_store->tab_detached(*moved_tab_id);
            tab.clear_session_tab_id();
        }
    }

    if (tab.is_loaded()) {
        tab.view().set_device_pixel_ratio(m_screen_observer->device_pixel_ratio());
        tab.view().set_display_metadata(m_screen_observer->display_id(), m_screen_observer->refresh_rate());
    }

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

    if (is_private() != target_window.is_private())
        return;

    cancel_window_close_preflight();

    auto* tab = m_tabs_container->tab(index);
    if (tab->is_loaded())
        tab->view().prepare_for_window_move();
    uninitialize_tab(tab);
    m_tabs_container->take_tab(index);
    if (m_current_tab == tab)
        set_current_tab(m_tabs_container->count() > 0 ? m_tabs_container->tab(m_tabs_container->current_index()) : nullptr);

    target_window.adopt_tab(*tab, target_index);
    sync_session_tab_order();

    if (m_tabs_container->count() == 0)
        close();
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

    auto& window = Application::the().new_window({}, configuration, IsPopupWindow::No, m_is_private, nullptr, {}, ShowWindow::No);
    move_tab_to_window(index, window, 0);

    if (configuration.maximized == true)
        window.showMaximized();
    else
        window.show();

    window.activateWindow();
    window.raise();
}

void BrowserWindow::set_current_tab(Tab* tab)
{
    if (tab == m_current_tab)
        return;

    // A restored tab loads only once it's selected.
    if (tab)
        tab->ensure_loaded();

    if (m_current_tab)
        m_current_tab->view().set_system_visibility_state(Web::HTML::VisibilityState::Hidden);

    m_current_tab = tab;

    if (m_current_tab) {
        m_current_tab->view().set_system_visibility_state(Web::HTML::VisibilityState::Visible);

        if (auto session_tab_id = m_current_tab->session_tab_id(); session_tab_id.has_value())
            m_session->session_store->active_tab_changed(*session_tab_id);
    }

    WebView::Application::the().update_actions_for_active_web_view();
}

void BrowserWindow::activate_tab(int index)
{
    m_tabs_container->set_current_index(index);
}

Tab* BrowserWindow::activate_tab_with_url(URL::URL const& url, URL::ExcludeFragment exclude_fragment)
{
    for (int index = 0; index < m_tabs_container->count(); ++index) {
        auto* tab = m_tabs_container->tab(index);
        if (tab && tab->url().equals(url, exclude_fragment)) {
            m_tabs_container->set_current_index(index);
            return tab;
        }
    }
    return nullptr;
}

bool BrowserWindow::definitely_close_tab(int index)
{
    cancel_window_close_preflight();

    if (m_tabs_container->count() == 1 && Application::the().file_downloader().has_active_downloads() && visible_browser_window_count() <= 1) {
        if (!Application::the().confirm_stop_active_downloads(this))
            return false;
    }

    auto* tab = m_tabs_container->tab(index);
    m_tabs_container->remove_tab(index);
    if (auto session_tab_id = tab->session_tab_id(); session_tab_id.has_value()) {
        WebView::SessionStore::TabClosed closed {
            .tab_id = *session_tab_id,
            .closed_at = UnixDateTime::now(),
        };
        if (auto result = m_session->session_store->tab_closed(AK::move(closed)); result.is_error())
            dbgln("Unable to record the closed tab in the session store: {}", result.error());
    }
    Application::the().update_reopen_recently_closed_actions();
    tab->deleteLater();

    if (m_tabs_container->count() == 0)
        close();

    return true;
}

void BrowserWindow::move_tab(int old_index, int new_index)
{
    auto* tab = m_tabs_container->tab(old_index);
    if (!tab)
        return;

    auto pinned_count = pinned_tab_count();
    if (tab->is_pinned())
        new_index = clamp(new_index, 0, pinned_count - 1);
    else
        new_index = clamp(new_index, pinned_count, m_tabs_container->count() - 1);
    if (new_index != old_index)
        m_tabs_container->tab_bar()->moveTab(old_index, new_index);
}

int BrowserWindow::pinned_tab_count() const
{
    return m_tabs_container->tab_bar()->pinned_tab_count();
}

int BrowserWindow::constrain_insertion_index(int index, bool pinned) const
{
    auto pinned_count = pinned_tab_count();
    if (pinned)
        return clamp(index, 0, pinned_count);
    return clamp(index, pinned_count, m_tabs_container->count());
}

void BrowserWindow::pin_tab(Tab& tab, Optional<int> pinned_index)
{
    auto index = tab_index(&tab);
    if (index < 0 || tab.is_pinned())
        return;

    auto pinned_count = pinned_tab_count();
    auto destination = clamp(pinned_index.value_or(pinned_count), 0, pinned_count);
    tab.set_pinned(true);
    if (index != destination)
        m_tabs_container->tab_bar()->moveTab(index, destination);
    pinned_state_changed(tab);
}

void BrowserWindow::unpin_tab(Tab& tab)
{
    auto index = tab_index(&tab);
    if (index < 0 || !tab.is_pinned())
        return;

    auto destination = pinned_tab_count() - 1;
    tab.set_pinned(false);
    if (index != destination)
        m_tabs_container->tab_bar()->moveTab(index, destination);
    pinned_state_changed(tab);
}

void BrowserWindow::pinned_state_changed(Tab& tab)
{
    // A move already reported the new order, through the tab bar's tabMoved signal.
    if (auto session_tab_id = tab.session_tab_id(); session_tab_id.has_value()) {
        if (auto result = m_session->session_store->tab_pinned_changed(*session_tab_id, tab.is_pinned()); result.is_error())
            dbgln("Unable to record the tab's pin in the session store: {}", result.error());
    }
    update_tab_close_button(tab);
    m_tabs_container->update_tab_layout();
}

void BrowserWindow::activate_first_unpinned_tab()
{
    if (auto first_unpinned = pinned_tab_count(); first_unpinned < m_tabs_container->count())
        activate_tab(first_unpinned);
}

void BrowserWindow::register_window_with_session_store()
{
    if (auto session_window_id = m_session->session_store->window_opened(); session_window_id.is_error())
        dbgln("Unable to register the new window with the session store: {}", session_window_id.error());
    else
        m_session_window_id = session_window_id.value();
}

void BrowserWindow::sync_session_tab_order()
{
    if (!m_session_window_id.has_value())
        return;

    Vector<WebView::SessionTabId> tab_order;
    tab_order.ensure_capacity(m_tabs_container->count());
    for (int i = 0; i < m_tabs_container->count(); ++i) {
        if (auto session_tab_id = m_tabs_container->tab(i)->session_tab_id(); session_tab_id.has_value())
            tab_order.append(*session_tab_id);
    }

    WebView::SessionStore::TabOrderChanged changed {
        .window_id = *m_session_window_id,
        .ordered_tabs = AK::move(tab_order),
    };
    m_session->session_store->tab_order_changed(AK::move(changed));
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

void BrowserWindow::update_window_title(QString const& title)
{
    char const* format = is_private() == WebView::IsPrivate::Yes
        ? "%1 - Ladybird (Private Browsing)"
        : "%1 - Ladybird";
    setWindowTitle(QString(format).arg(title));
}

void BrowserWindow::tab_title_changed(int index, QString const& title)
{
    m_tabs_container->set_tab_text(index, title);
    m_tabs_container->set_tab_tooltip(index, title);

    if (m_tabs_container->current_index() == index)
        update_window_title(title);
}

void BrowserWindow::tab_favicon_changed(int index, QIcon const& icon)
{
    if (index < 0)
        return;
    m_tabs_container->set_tab_icon(index, icon);
}

void BrowserWindow::update_chrome_style()
{
    menuBar()->setStyleSheet(ChromeStyle::menu_bar_style_sheet(palette()));
    m_hamburger_menu->setStyleSheet(ChromeStyle::hamburger_style_sheet(palette()));
}

TabBarButton* BrowserWindow::create_tab_close_button(Tab& tab)
{
    auto* close_button = new TabBarButton(create_chrome_icon(ChromeIcon::TabClose, palette()));
    close_button->setToolTip("Close Tab");
    connect(close_button, &QPushButton::clicked, this, [this, tab = &tab]() {
        request_to_close_tab(m_tabs_container->index_of(tab));
    });
    return close_button;
}

void BrowserWindow::initialize_tab_buttons(Tab* tab)
{
    auto index = m_tabs_container->index_of(tab);
    m_tabs_container->set_tab_icon(index, tab->tab_icon());

    m_tabs_container->tab_bar()->setTabButton(index, AUDIO_STATE_BUTTON_POSITION, nullptr);
    // A pinned tab has no close button.
    m_tabs_container->tab_bar()->setTabButton(index, TAB_CLOSE_BUTTON_POSITION, tab->is_pinned() ? nullptr : create_tab_close_button(*tab));
    m_tabs_container->update_tab_button_visibility();
}

void BrowserWindow::update_tab_close_button(Tab& tab)
{
    auto index = m_tabs_container->index_of(&tab);
    if (index < 0)
        return;

    auto* tab_bar = m_tabs_container->tab_bar();
    auto* close_button = tab_bar->tabButton(index, TAB_CLOSE_BUTTON_POSITION);
    if (tab.is_pinned() && close_button) {
        tab_bar->setTabButton(index, TAB_CLOSE_BUTTON_POSITION, nullptr);
        close_button->deleteLater();
    } else if (!tab.is_pinned() && !close_button) {
        tab_bar->setTabButton(index, TAB_CLOSE_BUTTON_POSITION, create_tab_close_button(tab));
    }
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
    if constexpr (!use_native_macos_window_controls()) {
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
}

void BrowserWindow::update_menu_bar_visibility()
{
    auto show_menu_bar = show_menu_bar_option_available() && Application::settings().appearance().show_menu_bar;
    menuBar()->setVisible(show_menu_bar);

    if (m_menu_bar_window_controls)
        m_menu_bar_window_controls->setVisible(show_menu_bar && uses_client_side_decorations());
    m_tabs_container->set_window_controls_visible(!show_menu_bar && has_chrome_in_titlebar());
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

void BrowserWindow::set_always_on_top(bool always_on_top)
{
    auto flags = windowFlags();
    flags.setFlag(Qt::WindowStaysOnTopHint, always_on_top);

    // NB: setWindowFlags() hides the window, so update the flags of the existing native window instead.
    overrideWindowFlags(flags);
    if (auto* handle = windowHandle())
        handle->setFlags(flags);
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

    m_restore_to_maximized = isMaximized();
    showFullScreen();

    for_each_tab([](Tab& tab) {
        tab.update_bookmarks_bar_visibility();
    });
}

void BrowserWindow::exit_fullscreen()
{
    if (!current_tab())
        return;

    m_tabs_container->set_tab_bar_visible(true);

    if (m_restore_to_maximized)
        showMaximized();
    else
        showNormal();

    for_each_tab([](Tab& tab) {
        tab.update_bookmarks_bar_visibility();
    });

    // A crash report that came in while the toolbar was hidden can be offered now, even on a tab other than the one
    // that was current back then.
    QTimer::singleShot(0, this, [] {
        Application::the().crash_report_notifier().show_pending();
    });
}

bool BrowserWindow::event(QEvent* event)
{
#if defined(AK_OS_MACOS)
    // NB: Qt sends the show event before the native window appears, so it never shows with its controls unmoved.
    if (event->type() == QEvent::Show) {
        hide_appkit_window_title(*this);
        offset_appkit_window_controls(*this, NATIVE_WINDOW_CONTROL_X_OFFSET, NATIVE_WINDOW_CONTROL_Y_OFFSET);
    }
    // NB: Qt sends this right after it sets the native title, which also puts the controls back in their default place.
    if (event->type() == QEvent::WindowTitleChange)
        reapply_appkit_window_control_offsets(*this);
    if (event->type() == QEvent::PlatformSurface) {
        auto* platform_surface_event = static_cast<QPlatformSurfaceEvent*>(event);
        if (platform_surface_event->surfaceEventType() == QPlatformSurfaceEvent::SurfaceCreated) {
            QTimer::singleShot(0, this, [this] {
                hide_appkit_window_title(*this);
                offset_appkit_window_controls(*this, NATIVE_WINDOW_CONTROL_X_OFFSET, NATIVE_WINDOW_CONTROL_Y_OFFSET);
            });
        }
    }
#endif

    if (event->type() == QEvent::WindowActivate) {
        Application::the().set_active_window(*this);
        WebView::Application::the().update_actions_for_active_web_view();
        // A quit raises and closes windows in its own order, so the restore order keeps how they were used before it.
        if (!Application::the().is_quit_in_progress())
            report_activation_to_session_store();
        QTimer::singleShot(0, this, [this] {
            refresh_resize_cursor_at_current_position(true);
            Application::the().crash_report_notifier().show_pending();
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
    if (!has_chrome_in_titlebar())
        return QMainWindow::eventFilter(object, event);

    if (event->type() == QEvent::ContextMenu) {
        auto* context_menu_event = static_cast<QContextMenuEvent*>(event);
        auto position = context_menu_event->pos();
        auto is_empty_menu_bar_area = widget == menuBar() && menuBar()->actionAt(position) == nullptr;
        if (!is_empty_window_drag_region(*widget, position) && !is_empty_menu_bar_area)
            return QMainWindow::eventFilter(object, event);

        show_window_context_menu(context_menu_event->globalPos());
        return true;
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
    auto edges = uses_client_side_decorations() ? resize_edges_for_position(position) : Qt::Edges {};
    if (event->type() == QEvent::MouseButtonPress && !isMaximized() && !isFullScreen()) {
        if (edges != Qt::Edges {}) {
            auto* handle = windowHandle();
            if (handle && handle->startSystemResize(edges))
                return true;
        }
    }

    if (is_empty_window_drag_region(*widget, mouse_event->position().toPoint()) && !isFullScreen()) {
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

Qt::Edges BrowserWindow::resize_edges_for_position(QPoint const& position) const
{
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

    for_each_tab([&](auto& tab) {
        tab.view().set_window_size({ width(), height() });
    });

    report_session_window_geometry();
}

void BrowserWindow::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::PaletteChange) {
        update_chrome_style();
        update_menu_bar_window_control_icons();
        update_tab_button_icons();
    } else if (event->type() == QEvent::WindowStateChange) {
        update_menu_bar_window_control_icons();
        m_tabs_container->update_window_button_icons();
        update_window_border();
        report_session_window_geometry();

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

void BrowserWindow::appearance_changed()
{
    update_menu_bar_visibility();

    for_each_tab([](Tab& tab) {
        tab.update_bookmarks_bar_visibility();
    });
}

void BrowserWindow::config_variable_changed(WebView::ConfigVariableID variable)
{
    if (variable == WebView::ConfigVariableID::UseClientSideWindowDecorations)
        update_window_decoration_state();
}

bool BrowserWindow::should_draw_window_border() const
{
    return windowFlags().testFlag(Qt::FramelessWindowHint) && !isFullScreen() && !isMaximized();
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

    report_session_window_geometry();
}

void BrowserWindow::wheelEvent(QWheelEvent* event)
{
    if (!m_current_tab)
        return;

    if (wheel_event_scrolls_continuously(*event))
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
    if (!m_window_close_approved) {
        if (m_window_close_coordinator.is_running()) {
            event->ignore();
            return;
        }

        if (Application::the().file_downloader().has_active_downloads()) {
            if (visible_browser_window_count() <= 1) {
                if (!Application::the().confirm_stop_active_downloads(this)) {
                    event->ignore();
                    return;
                }
            }
        }

        begin_window_close_preflight();
        event->ignore();
        return;
    }

    Vector<Function<void()>> close_requests;
    for_each_tab([&](auto& tab) {
        close_requests.append(tab.prepare_for_window_close());
    });
    for (auto& close_request : close_requests)
        close_request();

    clear_resize_cursor();

    auto active_tab_index = m_window_close_active_tab_index.value_or(static_cast<i64>(max(m_tabs_container->current_index(), 0)));

    if (m_is_private == WebView::IsPrivate::No) {
        if (m_is_popup_window == IsPopupWindow::No) {
            WebView::Application::settings().set_window_settings({
                .position = Gfx::IntPoint { pos().x(), pos().y() },
                .size = Gfx::IntSize { width(), height() },
                .maximized = isMaximized(),
            });
        }
    }

    QObject::deleteLater();

    QMainWindow::closeEvent(event);

    if (event->isAccepted() && m_session_window_id.has_value()) {
        WebView::SessionStore::WindowClosing closing {
            .window_id = *m_session_window_id,
            .active_tab_index = active_tab_index,
            .closed_at = UnixDateTime::now(),
        };
        if (auto result = m_session->session_store->window_closing(AK::move(closing)); result.is_error())
            dbgln("Unable to record the closing window in the session store: {}", result.error());
        Application::the().update_reopen_recently_closed_actions();
    }
}

void BrowserWindow::request_close_preflight(Function<void(bool approved)> on_complete)
{
    if (m_window_close_approved) {
        on_complete(true);
        return;
    }

    // A close the user already started on this window gives way to the caller's check, which asks every page again.
    if (m_window_close_coordinator.is_running())
        cancel_window_close_preflight();

    begin_window_close_preflight(AK::move(on_complete));
}

void BrowserWindow::raise_for_close_prompt()
{
    // Only an application-wide check carries a completion; a close the user started on this window is already in front.
    if (!m_window_close_coordinator.is_running() || !m_window_close_preflight_on_complete)
        return;

    // A page's beforeunload prompt shows inside this window, which may sit behind other windows or in the Dock while a
    // close check asks it, so the window comes to the front first, where the user can see the prompt.
    if (isMinimized())
        setWindowState(windowState() & ~Qt::WindowMinimized);
    raise();
    activateWindow();
}

void BrowserWindow::report_activation_to_session_store()
{
    if (m_session_window_id.has_value())
        m_session->session_store->window_activated(*m_session_window_id);
}

void BrowserWindow::report_session_window_geometry()
{
    if (!m_session_window_id.has_value())
        return;

    // Entering full screen clears the maximized state, and the window then fills the screen — so the last report from
    // before it, with its normal rect and maximized flag, stands as the window's saved geometry.
    if (isFullScreen())
        return;

    // A maximized window keeps the geometry it had before, for when it's restored.
    auto rect = isMaximized() ? normalGeometry() : geometry();
    WebView::SessionWindowGeometry window_geometry {
        .x = rect.x(),
        .y = rect.y(),
        .width = rect.width(),
        .height = rect.height(),
        .maximized = isMaximized(),
    };
    m_session->session_store->window_geometry_changed(*m_session_window_id, window_geometry);
}

void BrowserWindow::begin_window_close_preflight(Function<void(bool approved)> on_complete)
{
    m_window_close_active_tab_index = static_cast<i64>(max(m_tabs_container->current_index(), 0));
    m_window_close_original_active_tab = m_current_tab;
    m_window_close_current_preflight_tab = nullptr;
    m_window_close_approved_tabs.clear();

    Vector<QPointer<Tab>> tabs;
    if (m_current_tab)
        tabs.append(m_current_tab);
    for_each_tab([&](auto& tab) {
        if (&tab != m_current_tab)
            tabs.append(&tab);
    });

    Vector<WindowCloseCoordinator::Preflight> preflights;
    preflights.ensure_capacity(tabs.size());
    for (auto tab : tabs) {
        preflights.append([this, tab](Function<void(bool)> on_complete) mutable {
            if (!tab) {
                on_complete(true);
                return;
            }

            auto index = tab_index(tab);
            if (index < 0) {
                on_complete(true);
                return;
            }

            activate_tab(index);
            m_window_close_current_preflight_tab = tab;
            auto weak_this = QPointer<BrowserWindow>(this);
            tab->request_window_close_preflight([weak_this, tab, on_complete = AK::move(on_complete)](bool approved) mutable {
                if (!weak_this) {
                    on_complete(approved);
                    return;
                }
                if (weak_this->m_window_close_current_preflight_tab == tab)
                    weak_this->m_window_close_current_preflight_tab = nullptr;
                if (approved && tab && !weak_this->m_window_close_approved_tabs.contains([&](auto const& approved_tab) { return approved_tab == tab; }))
                    weak_this->m_window_close_approved_tabs.append(tab);
                on_complete(approved);
            });
        });
    }

    // With a completion, approval only marks the window as approved (so that a later close() goes straight through),
    // and the caller decides when to close it.
    m_window_close_preflight_on_complete = AK::move(on_complete);

    auto weak_this = QPointer<BrowserWindow>(this);
    m_window_close_coordinator.start(
        AK::move(preflights),
        [weak_this] {
            if (!weak_this)
                return;
            weak_this->m_window_close_approved = true;
            if (auto on_complete = AK::move(weak_this->m_window_close_preflight_on_complete)) {
                // The window stays open until the caller closes it, so it goes back to the tab it showed before the
                // check, which the session store then keeps as its active tab.
                if (auto* original_tab = weak_this->m_window_close_original_active_tab.data()) {
                    auto index = weak_this->tab_index(original_tab);
                    if (index >= 0)
                        weak_this->activate_tab(index);
                }
                on_complete(true);
                return;
            }
            QTimer::singleShot(0, weak_this, [weak_this] {
                if (weak_this && weak_this->m_window_close_approved)
                    weak_this->close();
            });
        },
        [weak_this] {
            if (!weak_this)
                return;
            if (auto* original_tab = weak_this->m_window_close_original_active_tab.data()) {
                auto index = weak_this->tab_index(original_tab);
                if (index >= 0)
                    weak_this->activate_tab(index);
            }
            weak_this->m_window_close_active_tab_index.clear();
            weak_this->m_window_close_original_active_tab = nullptr;
            weak_this->m_window_close_current_preflight_tab = nullptr;
            weak_this->m_window_close_approved_tabs.clear();
            weak_this->for_each_tab([](auto& tab) {
                tab.cancel_window_close_preflight();
            });
            if (auto on_complete = AK::move(weak_this->m_window_close_preflight_on_complete))
                on_complete(false);
        });
}

void BrowserWindow::cancel_window_close_preflight()
{
    if (m_window_close_coordinator.is_running()) {
        m_window_close_coordinator.cancel();
        return;
    }

    if (!m_window_close_approved)
        return;

    if (auto* original_tab = m_window_close_original_active_tab.data()) {
        auto index = tab_index(original_tab);
        if (index >= 0)
            activate_tab(index);
    }
    m_window_close_approved = false;
    m_window_close_active_tab_index.clear();
    m_window_close_original_active_tab = nullptr;
    m_window_close_current_preflight_tab = nullptr;
    m_window_close_approved_tabs.clear();
    for_each_tab([](auto& tab) {
        tab.cancel_window_close_preflight();
    });
}

void BrowserWindow::on_tab_load_start(Tab& tab)
{
    if (!m_window_close_coordinator.is_running() && !m_window_close_approved)
        return;

    if (m_window_close_current_preflight_tab == &tab || m_window_close_approved_tabs.contains([&](auto const& approved_tab) { return approved_tab == &tab; }))
        cancel_window_close_preflight();
}

}
