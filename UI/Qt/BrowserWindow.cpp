/*
 * Copyright (c) 2022-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Matthew Costa <ucosty@gmail.com>
 * Copyright (c) 2022, Filiph Sandström <filiph.sandstrom@filfatstudios.com>
 * Copyright (c) 2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <LibWeb/CSS/PreferredColorScheme.h>
#include <LibWeb/CSS/PreferredContrast.h>
#include <LibWeb/CSS/PreferredMotion.h>
#include <LibWeb/Loader/UserAgent.h>
#include <LibWebView/Application.h>
#include <LibWebView/CookieJar.h>
#include <LibWebView/UserAgent.h>
#include <UI/Qt/Application.h>
#include <UI/Qt/BrowserWindow.h>
#include <UI/Qt/Icon.h>
#include <UI/Qt/Settings.h>
#include <UI/Qt/SettingsDialog.h>
#include <UI/Qt/StringUtils.h>
#include <UI/Qt/TabBar.h>
#include <UI/Qt/TaskManagerWindow.h>
#include <UI/Qt/WebContentView.h>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QGuiApplication>
#include <QInputDialog>
#include <QPlainTextEdit>
#include <QShortcut>
#include <QTabBar>
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
    setWindowIcon(app_icon());

    // Listen for DPI changes
    m_device_pixel_ratio = devicePixelRatio();
    m_current_screen = screen();
    if (QT_VERSION < QT_VERSION_CHECK(6, 6, 0) || QGuiApplication::platformName() != "wayland") {
        setAttribute(Qt::WA_NativeWindow);
        setAttribute(Qt::WA_DontCreateNativeAncestors);
        QObject::connect(m_current_screen, &QScreen::logicalDotsPerInchChanged, this, &BrowserWindow::device_pixel_ratio_changed);
        QObject::connect(windowHandle(), &QWindow::screenChanged, this, [this](QScreen* screen) {
            if (m_device_pixel_ratio != devicePixelRatio())
                device_pixel_ratio_changed(devicePixelRatio());

            // Listen for logicalDotsPerInchChanged signals on new screen
            QObject::disconnect(m_current_screen, &QScreen::logicalDotsPerInchChanged, nullptr, nullptr);
            m_current_screen = screen;
            QObject::connect(m_current_screen, &QScreen::logicalDotsPerInchChanged, this, &BrowserWindow::device_pixel_ratio_changed);
        });
    }

    QObject::connect(Settings::the(), &Settings::enable_do_not_track_changed, this, [this](bool enable) {
        for_each_tab([enable](auto& tab) {
            tab.set_enable_do_not_track(enable);
        });
    });

    QObject::connect(Settings::the(), &Settings::enable_autoplay_changed, this, [this](bool enable) {
        for_each_tab([enable](auto& tab) {
            tab.set_enable_autoplay(enable);
        });
    });

    QObject::connect(Settings::the(), &Settings::preferred_languages_changed, this, [this](QStringList languages) {
        Vector<String> preferred_languages;
        preferred_languages.ensure_capacity(languages.length());
        for (auto& language : languages) {
            preferred_languages.append(ak_string_from_qstring(language));
        }

        for_each_tab([preferred_languages](auto& tab) {
            tab.set_preferred_languages(preferred_languages);
        });
    });

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

    m_copy_selection_action = new QAction("&Copy", this);
    m_copy_selection_action->setIcon(load_icon_from_uri("resource://icons/16x16/edit-copy.png"sv));
    m_copy_selection_action->setShortcuts(QKeySequence::keyBindings(QKeySequence::StandardKey::Copy));
    edit_menu->addAction(m_copy_selection_action);
    QObject::connect(m_copy_selection_action, &QAction::triggered, this, &BrowserWindow::copy_selected_text);

    m_paste_action = new QAction("&Paste", this);
    m_paste_action->setIcon(load_icon_from_uri("resource://icons/16x16/paste.png"sv));
    m_paste_action->setShortcuts(QKeySequence::keyBindings(QKeySequence::StandardKey::Paste));
    edit_menu->addAction(m_paste_action);
    QObject::connect(m_paste_action, &QAction::triggered, this, &BrowserWindow::paste);

    m_select_all_action = new QAction("Select &All", this);
    m_select_all_action->setIcon(load_icon_from_uri("resource://icons/16x16/select-all.png"sv));
    m_select_all_action->setShortcuts(QKeySequence::keyBindings(QKeySequence::StandardKey::SelectAll));
    edit_menu->addAction(m_select_all_action);
    QObject::connect(m_select_all_action, &QAction::triggered, this, &BrowserWindow::select_all);

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

    auto* settings_action = new QAction("&Settings", this);
    settings_action->setIcon(load_icon_from_uri("resource://icons/16x16/settings.png"sv));
    settings_action->setShortcuts(QKeySequence::keyBindings(QKeySequence::StandardKey::Preferences));
    edit_menu->addAction(settings_action);

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

    m_zoom_menu = view_menu->addMenu("&Zoom");

    auto* zoom_in_action = new QAction("Zoom &In", this);
    zoom_in_action->setIcon(load_icon_from_uri("resource://icons/16x16/zoom-in.png"sv));
    auto zoom_in_shortcuts = QKeySequence::keyBindings(QKeySequence::StandardKey::ZoomIn);
    auto secondary_zoom_shortcut = QKeySequence(Qt::CTRL | Qt::Key_Equal);
    if (!zoom_in_shortcuts.contains(secondary_zoom_shortcut))
        zoom_in_shortcuts.append(AK::move(secondary_zoom_shortcut));

    zoom_in_action->setShortcuts(zoom_in_shortcuts);
    m_zoom_menu->addAction(zoom_in_action);
    QObject::connect(zoom_in_action, &QAction::triggered, this, &BrowserWindow::zoom_in);

    auto* zoom_out_action = new QAction("Zoom &Out", this);
    zoom_out_action->setIcon(load_icon_from_uri("resource://icons/16x16/zoom-out.png"sv));
    zoom_out_action->setShortcuts(QKeySequence::keyBindings(QKeySequence::StandardKey::ZoomOut));
    m_zoom_menu->addAction(zoom_out_action);
    QObject::connect(zoom_out_action, &QAction::triggered, this, &BrowserWindow::zoom_out);

    auto* reset_zoom_action = new QAction("&Reset Zoom", this);
    reset_zoom_action->setIcon(load_icon_from_uri("resource://icons/16x16/zoom-reset.png"sv));
    reset_zoom_action->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_0));
    m_zoom_menu->addAction(reset_zoom_action);
    QObject::connect(reset_zoom_action, &QAction::triggered, this, &BrowserWindow::reset_zoom);

    view_menu->addSeparator();

    auto* color_scheme_menu = view_menu->addMenu("&Color Scheme");

    auto* color_scheme_group = new QActionGroup(this);

    auto* auto_color_scheme = new QAction("&Auto", this);
    auto_color_scheme->setCheckable(true);
    color_scheme_group->addAction(auto_color_scheme);
    color_scheme_menu->addAction(auto_color_scheme);
    QObject::connect(auto_color_scheme, &QAction::triggered, this, [this] {
        set_preferred_color_scheme(Web::CSS::PreferredColorScheme::Auto);
    });
    auto* light_color_scheme = new QAction("&Light", this);
    light_color_scheme->setCheckable(true);
    color_scheme_group->addAction(light_color_scheme);
    color_scheme_menu->addAction(light_color_scheme);
    QObject::connect(light_color_scheme, &QAction::triggered, this, [this] {
        set_preferred_color_scheme(Web::CSS::PreferredColorScheme::Light);
    });

    auto* dark_color_scheme = new QAction("&Dark", this);
    dark_color_scheme->setCheckable(true);
    color_scheme_group->addAction(dark_color_scheme);
    color_scheme_menu->addAction(dark_color_scheme);
    QObject::connect(dark_color_scheme, &QAction::triggered, this, [this] {
        set_preferred_color_scheme(Web::CSS::PreferredColorScheme::Dark);
    });

    auto_color_scheme->setChecked(true);

    auto* contrast_menu = view_menu->addMenu("&Contrast");

    auto* contrast_group = new QActionGroup(this);

    auto* auto_contrast = new QAction("&Auto", this);
    auto_contrast->setCheckable(true);
    contrast_group->addAction(auto_contrast);
    contrast_menu->addAction(auto_contrast);
    QObject::connect(auto_contrast, &QAction::triggered, this, &BrowserWindow::enable_auto_contrast);

    auto* less_contrast = new QAction("&Less", this);
    less_contrast->setCheckable(true);
    contrast_group->addAction(less_contrast);
    contrast_menu->addAction(less_contrast);
    QObject::connect(less_contrast, &QAction::triggered, this, &BrowserWindow::enable_less_contrast);

    auto* more_contrast = new QAction("&More", this);
    more_contrast->setCheckable(true);
    contrast_group->addAction(more_contrast);
    contrast_menu->addAction(more_contrast);
    QObject::connect(more_contrast, &QAction::triggered, this, &BrowserWindow::enable_more_contrast);

    auto* no_preference_contrast = new QAction("&No Preference", this);
    no_preference_contrast->setCheckable(true);
    contrast_group->addAction(no_preference_contrast);
    contrast_menu->addAction(no_preference_contrast);
    QObject::connect(no_preference_contrast, &QAction::triggered, this, &BrowserWindow::enable_no_preference_contrast);

    auto_contrast->setChecked(true);

    auto* motion_menu = view_menu->addMenu("&Motion");

    auto* motion_group = new QActionGroup(this);

    auto* auto_motion = new QAction("&Auto", this);
    auto_motion->setCheckable(true);
    motion_group->addAction(auto_motion);
    motion_menu->addAction(auto_motion);
    QObject::connect(auto_motion, &QAction::triggered, this, &BrowserWindow::enable_auto_motion);

    auto* reduce_motion = new QAction("&Reduce", this);
    reduce_motion->setCheckable(true);
    motion_group->addAction(reduce_motion);
    motion_menu->addAction(reduce_motion);
    QObject::connect(reduce_motion, &QAction::triggered, this, &BrowserWindow::enable_reduce_motion);

    auto* no_preference_motion = new QAction("&No Preference", this);
    no_preference_motion->setCheckable(true);
    motion_group->addAction(no_preference_motion);
    motion_menu->addAction(no_preference_motion);
    QObject::connect(no_preference_motion, &QAction::triggered, this, &BrowserWindow::enable_no_preference_motion);

    auto_motion->setChecked(true);

    auto* show_menubar = new QAction("Show &Menubar", this);
    show_menubar->setCheckable(true);
    show_menubar->setChecked(Settings::the()->show_menubar());
    view_menu->addAction(show_menubar);
    QObject::connect(show_menubar, &QAction::triggered, this, [](bool checked) {
        Settings::the()->set_show_menubar(checked);
    });

    auto* inspect_menu = m_hamburger_menu->addMenu("&Inspect");
    menuBar()->addMenu(inspect_menu);

    m_view_source_action = new QAction("View &Source", this);
    m_view_source_action->setIcon(load_icon_from_uri("resource://icons/16x16/filetype-html.png"sv));
    m_view_source_action->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_U));
    inspect_menu->addAction(m_view_source_action);
    QObject::connect(m_view_source_action, &QAction::triggered, this, [this] {
        if (m_current_tab) {
            m_current_tab->view().get_source();
        }
    });

    auto* task_manager_action = new QAction("Open Task &Manager", this);
    task_manager_action->setIcon(load_icon_from_uri("resource://icons/16x16/app-system-monitor.png"sv));
    task_manager_action->setShortcuts({ QKeySequence("Ctrl+Shift+M") });
    inspect_menu->addAction(task_manager_action);
    QObject::connect(task_manager_action, &QAction::triggered, this, [&] {
        static_cast<Ladybird::Application*>(QApplication::instance())->show_task_manager_window();
    });

    auto* debug_menu = m_hamburger_menu->addMenu("&Debug");
    menuBar()->addMenu(debug_menu);

    auto* dump_session_history_tree_action = new QAction("Dump Session History Tree", this);
    dump_session_history_tree_action->setIcon(load_icon_from_uri("resource://icons/16x16/history.png"sv));
    debug_menu->addAction(dump_session_history_tree_action);
    QObject::connect(dump_session_history_tree_action, &QAction::triggered, this, [this] {
        debug_request("dump-session-history");
    });

    auto* dump_dom_tree_action = new QAction("Dump &DOM Tree", this);
    dump_dom_tree_action->setIcon(load_icon_from_uri("resource://icons/browser/dom-tree.png"sv));
    debug_menu->addAction(dump_dom_tree_action);
    QObject::connect(dump_dom_tree_action, &QAction::triggered, this, [this] {
        debug_request("dump-dom-tree");
    });

    auto* dump_layout_tree_action = new QAction("Dump &Layout Tree", this);
    dump_layout_tree_action->setIcon(load_icon_from_uri("resource://icons/16x16/layout.png"sv));
    debug_menu->addAction(dump_layout_tree_action);
    QObject::connect(dump_layout_tree_action, &QAction::triggered, this, [this] {
        debug_request("dump-layout-tree");
    });

    auto* dump_paint_tree_action = new QAction("Dump &Paint Tree", this);
    dump_paint_tree_action->setIcon(load_icon_from_uri("resource://icons/16x16/layout.png"sv));
    debug_menu->addAction(dump_paint_tree_action);
    QObject::connect(dump_paint_tree_action, &QAction::triggered, this, [this] {
        debug_request("dump-paint-tree");
    });

    auto* dump_stacking_context_tree_action = new QAction("Dump S&tacking Context Tree", this);
    dump_stacking_context_tree_action->setIcon(load_icon_from_uri("resource://icons/16x16/layers.png"sv));
    debug_menu->addAction(dump_stacking_context_tree_action);
    QObject::connect(dump_stacking_context_tree_action, &QAction::triggered, this, [this] {
        debug_request("dump-stacking-context-tree");
    });

    auto* dump_style_sheets_action = new QAction("Dump &Style Sheets", this);
    dump_style_sheets_action->setIcon(load_icon_from_uri("resource://icons/16x16/filetype-css.png"sv));
    debug_menu->addAction(dump_style_sheets_action);
    QObject::connect(dump_style_sheets_action, &QAction::triggered, this, [this] {
        debug_request("dump-style-sheets");
    });

    auto* dump_styles_action = new QAction("Dump &All Resolved Styles", this);
    dump_styles_action->setIcon(load_icon_from_uri("resource://icons/16x16/filetype-css.png"sv));
    debug_menu->addAction(dump_styles_action);
    QObject::connect(dump_styles_action, &QAction::triggered, this, [this] {
        debug_request("dump-all-resolved-styles");
    });

    auto* dump_cookies_action = new QAction("Dump C&ookies", this);
    dump_cookies_action->setIcon(load_icon_from_uri("resource://icons/browser/cookie.png"sv));
    debug_menu->addAction(dump_cookies_action);
    QObject::connect(dump_cookies_action, &QAction::triggered, this, [] {
        WebView::Application::cookie_jar().dump_cookies();
    });

    auto* dump_local_storage_action = new QAction("Dump Loc&al Storage", this);
    dump_local_storage_action->setIcon(load_icon_from_uri("resource://icons/browser/local-storage.png"sv));
    debug_menu->addAction(dump_local_storage_action);
    QObject::connect(dump_local_storage_action, &QAction::triggered, this, [this] {
        debug_request("dump-local-storage");
    });

    debug_menu->addSeparator();

    m_show_line_box_borders_action = new QAction("Show Line Box Borders", this);
    m_show_line_box_borders_action->setCheckable(true);
    m_show_line_box_borders_action->setIcon(load_icon_from_uri("resource://icons/16x16/box.png"sv));
    debug_menu->addAction(m_show_line_box_borders_action);
    QObject::connect(m_show_line_box_borders_action, &QAction::triggered, this, [this] {
        bool state = m_show_line_box_borders_action->isChecked();
        for_each_tab([state](auto& tab) {
            tab.set_line_box_borders(state);
        });
    });

    debug_menu->addSeparator();

    auto* collect_garbage_action = new QAction("Collect &Garbage", this);
    collect_garbage_action->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_G));
    collect_garbage_action->setIcon(load_icon_from_uri("resource://icons/16x16/trash-can.png"sv));
    debug_menu->addAction(collect_garbage_action);
    QObject::connect(collect_garbage_action, &QAction::triggered, this, [this] {
        debug_request("collect-garbage");
    });

    auto* dump_gc_graph_action = new QAction("Dump GC graph", this);
    debug_menu->addAction(dump_gc_graph_action);
    QObject::connect(dump_gc_graph_action, &QAction::triggered, this, [this] {
        if (m_current_tab) {
            auto gc_graph_path = m_current_tab->view().dump_gc_graph();
            warnln("\033[33;1mDumped GC-graph into {}"
                   "\033[0m",
                gc_graph_path);
        }
    });

    auto* clear_cache_action = new QAction("Clear &Cache", this);
    clear_cache_action->setIcon(load_icon_from_uri("resource://icons/browser/clear-cache.png"sv));
    debug_menu->addAction(clear_cache_action);
    QObject::connect(clear_cache_action, &QAction::triggered, this, [this] {
        debug_request("clear-cache");
    });

    auto* clear_all_cookies_action = new QAction("Clear All Cookies", this);
    debug_menu->addAction(clear_all_cookies_action);
    QObject::connect(clear_all_cookies_action, &QAction::triggered, this, [] {
        WebView::Application::cookie_jar().clear_all_cookies();
    });

    auto* spoof_user_agent_menu = debug_menu->addMenu("Spoof &User Agent");
    spoof_user_agent_menu->setIcon(load_icon_from_uri("resource://icons/16x16/spoof.png"sv));

    auto* user_agent_group = new QActionGroup(this);

    auto add_user_agent = [this, &user_agent_group, &spoof_user_agent_menu](auto name, auto const& user_agent) {
        auto* action = new QAction(qstring_from_ak_string(name), this);
        action->setCheckable(true);
        user_agent_group->addAction(action);
        spoof_user_agent_menu->addAction(action);
        QObject::connect(action, &QAction::triggered, this, [this, user_agent] {
            for_each_tab([user_agent](auto& tab) {
                tab.set_user_agent_string(user_agent);
            });
            set_user_agent_string(user_agent);
        });
        return action;
    };

    auto const& user_agent_preset = WebView::Application::web_content_options().user_agent_preset;
    set_user_agent_string(user_agent_preset.has_value() ? *WebView::user_agents.get(*user_agent_preset) : Web::default_user_agent);

    auto* disable_spoofing = add_user_agent("Disabled"sv, Web::default_user_agent);
    disable_spoofing->setChecked(!user_agent_preset.has_value());
    for (auto const& user_agent : WebView::user_agents) {
        auto* spoofed_user_agent = add_user_agent(user_agent.key, user_agent.value.to_byte_string());
        spoofed_user_agent->setChecked(user_agent.key == user_agent_preset);
    }

    auto* custom_user_agent_action = new QAction("Custom...", this);
    custom_user_agent_action->setCheckable(true);
    user_agent_group->addAction(custom_user_agent_action);
    spoof_user_agent_menu->addAction(custom_user_agent_action);
    QObject::connect(custom_user_agent_action, &QAction::triggered, this, [this, disable_spoofing] {
        auto user_agent = QInputDialog::getText(this, "Custom User Agent", "Enter User Agent:");
        if (!user_agent.isEmpty()) {
            auto user_agent_byte_string = ak_byte_string_from_qstring(user_agent);
            for_each_tab([&](auto& tab) {
                tab.set_user_agent_string(user_agent_byte_string);
            });
            set_user_agent_string(user_agent_byte_string);
        } else {
            disable_spoofing->activate(QAction::Trigger);
        }
    });

    auto* navigator_compatibility_mode_menu = debug_menu->addMenu("Navigator Compatibility Mode");
    navigator_compatibility_mode_menu->setIcon(load_icon_from_uri("resource://icons/16x16/spoof.png"sv));

    auto* navigator_compatibility_mode_group = new QActionGroup(this);

    auto add_navigator_compatibility_mode = [this, &navigator_compatibility_mode_group, &navigator_compatibility_mode_menu](auto name, auto const& compatibility_mode) {
        auto* action = new QAction(qstring_from_ak_string(name), this);
        action->setCheckable(true);
        navigator_compatibility_mode_group->addAction(action);
        navigator_compatibility_mode_menu->addAction(action);
        QObject::connect(action, &QAction::triggered, this, [this, compatibility_mode] {
            for_each_tab([compatibility_mode](auto& tab) {
                tab.set_navigator_compatibility_mode(compatibility_mode);
            });
            set_navigator_compatibility_mode(compatibility_mode);
        });
        return action;
    };
    auto* chrome_compatibility_mode = add_navigator_compatibility_mode("Chrome"_string, "chrome"sv.to_byte_string());
    chrome_compatibility_mode->setChecked(true);
    add_navigator_compatibility_mode("Gecko"_string, "gecko"sv.to_byte_string());
    add_navigator_compatibility_mode("WebKit"_string, "webkit"sv.to_byte_string());
    set_navigator_compatibility_mode("chrome");

    debug_menu->addSeparator();

    m_enable_scripting_action = new QAction("Enable Scripting", this);
    m_enable_scripting_action->setCheckable(true);
    m_enable_scripting_action->setChecked(WebView::Application::chrome_options().disable_scripting == WebView::DisableScripting::No);
    debug_menu->addAction(m_enable_scripting_action);
    QObject::connect(m_enable_scripting_action, &QAction::triggered, this, [this] {
        bool state = m_enable_scripting_action->isChecked();
        for_each_tab([state](auto& tab) {
            tab.set_scripting(state);
        });
    });

    m_enable_content_filtering_action = new QAction("Enable Content Filtering", this);
    m_enable_content_filtering_action->setCheckable(true);
    m_enable_content_filtering_action->setChecked(true);
    debug_menu->addAction(m_enable_content_filtering_action);
    QObject::connect(m_enable_content_filtering_action, &QAction::triggered, this, [this] {
        bool const state = m_enable_content_filtering_action->isChecked();
        for_each_tab([state](auto& tab) {
            tab.set_content_filtering(state);
        });
    });

    m_block_pop_ups_action = new QAction("Block Pop-ups", this);
    m_block_pop_ups_action->setCheckable(true);
    m_block_pop_ups_action->setChecked(WebView::Application::chrome_options().allow_popups == WebView::AllowPopups::No);
    debug_menu->addAction(m_block_pop_ups_action);
    QObject::connect(m_block_pop_ups_action, &QAction::triggered, this, [this] {
        bool state = m_block_pop_ups_action->isChecked();
        for_each_tab([state](auto& tab) {
            tab.set_block_popups(state);
        });
    });

    m_enable_same_origin_policy_action = new QAction("Enable Same-Origin Policy", this);
    m_enable_same_origin_policy_action->setCheckable(true);
    debug_menu->addAction(m_enable_same_origin_policy_action);
    QObject::connect(m_enable_same_origin_policy_action, &QAction::triggered, this, [this] {
        bool state = m_enable_same_origin_policy_action->isChecked();
        for_each_tab([state](auto& tab) {
            tab.set_same_origin_policy(state);
        });
    });

    auto* help_menu = m_hamburger_menu->addMenu("&Help");
    menuBar()->addMenu(help_menu);

    auto* about_action = new QAction("&About Ladybird", this);
    help_menu->addAction(about_action);
    QObject::connect(about_action, &QAction::triggered, this, [this] {
        new_tab_from_url(URL::about_version(), Web::HTML::ActivateTab::Yes);
    });

    m_hamburger_menu->addSeparator();
    file_menu->addSeparator();

    auto* quit_action = new QAction("&Quit", this);
    quit_action->setShortcuts(QKeySequence::keyBindings(QKeySequence::StandardKey::Quit));
    m_hamburger_menu->addAction(quit_action);
    file_menu->addAction(quit_action);
    QObject::connect(quit_action, &QAction::triggered, this, &QMainWindow::close);

    QObject::connect(m_new_tab_action, &QAction::triggered, this, [this] {
        auto url = ak_url_from_qstring(Settings::the()->new_tab_page());
        VERIFY(url.has_value());
        auto& tab = new_tab_from_url(url.release_value(), Web::HTML::ActivateTab::Yes);
        tab.set_url_is_hidden(true);
        tab.focus_location_editor();
    });
    QObject::connect(m_new_window_action, &QAction::triggered, this, [] {
        (void)static_cast<Ladybird::Application*>(QApplication::instance())->new_window({});
    });
    QObject::connect(open_file_action, &QAction::triggered, this, &BrowserWindow::open_file);
    QObject::connect(settings_action, &QAction::triggered, this, [this] {
        if (!m_settings_dialog) {
            m_settings_dialog = new SettingsDialog(this);
        }

        m_settings_dialog->show();
        m_settings_dialog->setFocus();
    });
    QObject::connect(m_tabs_container, &QTabWidget::currentChanged, [this](int index) {
        auto* tab = as<Tab>(m_tabs_container->widget(index));
        if (tab)
            setWindowTitle(QString("%1 - Ladybird").arg(tab->title()));

        set_current_tab(tab);
    });
    QObject::connect(m_tabs_container, &QTabWidget::tabCloseRequested, this, &BrowserWindow::close_tab);
    QObject::connect(close_current_tab_action, &QAction::triggered, this, &BrowserWindow::close_current_tab);

    m_go_back_action = new QAction("Go Back", this);
    connect(m_go_back_action, &QAction::triggered, this, [this] {
        if (m_current_tab)
            m_current_tab->back();
    });
    m_go_forward_action = new QAction("Go Forward", this);
    connect(m_go_forward_action, &QAction::triggered, this, [this] {
        if (m_current_tab)
            m_current_tab->forward();
    });
    m_reload_action = new QAction("&Reload", this);
    connect(m_reload_action, &QAction::triggered, this, [this] {
        if (m_current_tab)
            m_current_tab->reload();
    });

    m_go_back_action->setShortcuts(QKeySequence::keyBindings(QKeySequence::StandardKey::Back));
    m_go_forward_action->setShortcuts(QKeySequence::keyBindings(QKeySequence::StandardKey::Forward));
    m_reload_action->setShortcuts(QKeySequence::keyBindings(QKeySequence::StandardKey::Refresh));
    m_go_back_action->setEnabled(false);
    m_go_forward_action->setEnabled(false);
    m_reload_action->setEnabled(true);

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
}

void BrowserWindow::set_current_tab(Tab* tab)
{
    m_current_tab = tab;
    if (tab) {
        update_displayed_zoom_level();
        tab->update_navigation_buttons_state();
    }
}

void BrowserWindow::debug_request(ByteString const& request, ByteString const& argument)
{
    if (!m_current_tab)
        return;
    m_current_tab->debug_request(request, argument);
}

Tab& BrowserWindow::new_tab_from_url(URL::URL const& url, Web::HTML::ActivateTab activate_tab)
{
    auto& tab = create_new_tab(activate_tab);
    tab.navigate(url);
    return tab;
}

Tab& BrowserWindow::new_tab_from_content(StringView html, Web::HTML::ActivateTab activate_tab)
{
    auto& tab = create_new_tab(activate_tab);
    tab.load_html(html);
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
    QObject::connect(tab, &Tab::navigation_buttons_state_changed, this, &BrowserWindow::tab_navigation_buttons_state_changed);

    QObject::connect(&tab->view(), &WebContentView::urls_dropped, this, [this](auto& urls) {
        VERIFY(urls.size());
        m_current_tab->navigate(ak_url_from_qurl(urls[0]));

        for (qsizetype i = 1; i < urls.size(); ++i)
            new_tab_from_url(ak_url_from_qurl(urls[i]), Web::HTML::ActivateTab::No);
    });

    tab->view().on_new_web_view = [this, tab](auto activate_tab, Web::HTML::WebViewHints hints, Optional<u64> page_index) {
        if (hints.popup) {
            auto& window = static_cast<Ladybird::Application*>(QApplication::instance())->new_window({}, IsPopupWindow::Yes, tab, AK::move(page_index));
            window.set_window_rect(hints.screen_x, hints.screen_y, hints.width, hints.height);
            return window.current_tab()->view().handle();
        }
        auto& new_tab = new_child_tab(activate_tab, *tab, page_index);
        return new_tab.view().handle();
    };

    tab->view().on_tab_open_request = [this](auto url, auto activate_tab) {
        auto& tab = new_tab_from_url(url, activate_tab);
        return tab.view().handle();
    };

    tab->view().on_link_click = [this](auto url, auto target, unsigned modifiers) {
        // TODO: maybe activate tabs according to some configuration, this is just normal current browser behavior
        if (modifiers == Web::UIEvents::Mod_Ctrl) {
            m_current_tab->view().on_tab_open_request(url, Web::HTML::ActivateTab::No);
        } else if (target == "_blank") {
            m_current_tab->view().on_tab_open_request(url, Web::HTML::ActivateTab::Yes);
        } else {
            m_current_tab->view().load(url);
        }
    };

    tab->view().on_link_middle_click = [this](auto url, auto target, unsigned modifiers) {
        m_current_tab->view().on_link_click(url, target, Web::UIEvents::Mod_Ctrl);
        (void)modifiers;
    };

    m_tabs_container->setTabIcon(m_tabs_container->indexOf(tab), tab->favicon());
    create_close_button_for_tab(tab);

    Vector<String> preferred_languages;
    preferred_languages.ensure_capacity(Settings::the()->preferred_languages().length());
    for (auto& language : Settings::the()->preferred_languages()) {
        preferred_languages.append(ak_string_from_qstring(language));
    }

    tab->set_line_box_borders(m_show_line_box_borders_action->isChecked());
    tab->set_scripting(m_enable_scripting_action->isChecked());
    tab->set_content_filtering(m_enable_content_filtering_action->isChecked());
    tab->set_block_popups(m_block_pop_ups_action->isChecked());
    tab->set_same_origin_policy(m_enable_same_origin_policy_action->isChecked());
    tab->set_user_agent_string(user_agent_string());
    tab->set_preferred_languages(preferred_languages);
    tab->set_navigator_compatibility_mode(navigator_compatibility_mode());
    tab->set_enable_do_not_track(Settings::the()->enable_do_not_track());
    tab->set_enable_autoplay(WebView::Application::web_content_options().enable_autoplay == WebView::EnableAutoplay::Yes || Settings::the()->enable_autoplay());
    tab->view().set_preferred_color_scheme(m_preferred_color_scheme);
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

void BrowserWindow::tab_navigation_buttons_state_changed(int index)
{
    auto* tab = as<Tab>(m_tabs_container->widget(index));
    tab->update_navigation_buttons_state();
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

void BrowserWindow::enable_auto_contrast()
{
    for_each_tab([](auto& tab) {
        tab.view().set_preferred_contrast(Web::CSS::PreferredContrast::Auto);
    });
}

void BrowserWindow::enable_less_contrast()
{
    for_each_tab([](auto& tab) {
        tab.view().set_preferred_contrast(Web::CSS::PreferredContrast::Less);
    });
}

void BrowserWindow::enable_more_contrast()
{
    for_each_tab([](auto& tab) {
        tab.view().set_preferred_contrast(Web::CSS::PreferredContrast::More);
    });
}

void BrowserWindow::enable_no_preference_contrast()
{
    for_each_tab([](auto& tab) {
        tab.view().set_preferred_contrast(Web::CSS::PreferredContrast::NoPreference);
    });
}

void BrowserWindow::enable_auto_motion()
{
    for_each_tab([](auto& tab) {
        tab.view().set_preferred_motion(Web::CSS::PreferredMotion::Auto);
    });
}

void BrowserWindow::enable_no_preference_motion()
{
    for_each_tab([](auto& tab) {
        tab.view().set_preferred_motion(Web::CSS::PreferredMotion::NoPreference);
    });
}

void BrowserWindow::enable_reduce_motion()
{
    for_each_tab([](auto& tab) {
        tab.view().set_preferred_motion(Web::CSS::PreferredMotion::Reduce);
    });
}

void BrowserWindow::zoom_in()
{
    if (!m_current_tab)
        return;
    m_current_tab->view().zoom_in();
    update_displayed_zoom_level();
}

void BrowserWindow::zoom_out()
{
    if (!m_current_tab)
        return;
    m_current_tab->view().zoom_out();
    update_displayed_zoom_level();
}

void BrowserWindow::reset_zoom()
{
    if (!m_current_tab)
        return;
    m_current_tab->view().reset_zoom();
    update_displayed_zoom_level();
}

void BrowserWindow::update_zoom_menu()
{
    VERIFY(m_zoom_menu);
    auto zoom_level_text = MUST(String::formatted("&Zoom ({}%)", round_to<int>(m_current_tab->view().zoom_level() * 100)));
    m_zoom_menu->setTitle(qstring_from_ak_string(zoom_level_text));
}

void BrowserWindow::select_all()
{
    if (!m_current_tab)
        return;

    m_current_tab->view().select_all();
}

void BrowserWindow::show_find_in_page()
{
    if (!m_current_tab)
        return;

    m_current_tab->show_find_in_page();
}

void BrowserWindow::paste()
{
    if (!m_current_tab)
        return;

    auto* clipboard = QGuiApplication::clipboard();
    m_current_tab->view().paste(ak_string_from_qstring(clipboard->text()));
}

void BrowserWindow::update_displayed_zoom_level()
{
    VERIFY(m_current_tab);
    update_zoom_menu();
    m_current_tab->update_reset_zoom_button();
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

void BrowserWindow::set_preferred_color_scheme(Web::CSS::PreferredColorScheme color_scheme)
{
    m_preferred_color_scheme = color_scheme;
    for_each_tab([color_scheme](auto& tab) {
        tab.view().set_preferred_color_scheme(color_scheme);
    });
}

void BrowserWindow::copy_selected_text()
{
    if (!m_current_tab)
        return;

    auto text = m_current_tab->view().selected_text();

    auto* clipboard = QGuiApplication::clipboard();
    clipboard->setText(qstring_from_ak_string(text));
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
        static_cast<Ladybird::Application*>(QApplication::instance())->set_active_window(*this);

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
    if ((event->modifiers() & Qt::ControlModifier) != 0) {
        if (event->angleDelta().y() > 0)
            zoom_in();
        else if (event->angleDelta().y() < 0)
            zoom_out();
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
