/*
 * Copyright (c) 2025-2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibURL/Parser.h>
#include <LibWebView/Application.h>
#include <LibWebView/HistoryStore.h>
#include <UI/Qt/Icon.h>
#include <UI/Qt/Menu.h>
#include <UI/Qt/StringUtils.h>
#include <UI/Qt/WebContentView.h>

#include <QAction>
#include <QMenu>
#include <QPointer>
#include <QToolButton>
#include <QWidget>

namespace Ladybird {

static constexpr auto DYNAMIC_HISTORY_MENU_ITEM_PROPERTY = "LadybirdDynamicHistoryMenuItem";
static constexpr size_t RECENT_HISTORY_MENU_ITEM_LIMIT = 15;

class ActionObserver final : public WebView::Action::Observer {
public:
    static NonnullOwnPtr<ActionObserver> create(WebView::Action& action, QAction& qaction, IncludeActionIcon include_action_icon)
    {
        return adopt_own(*new ActionObserver(action, qaction, include_action_icon));
    }

    virtual void on_text_changed(WebView::Action& action) override
    {
        if (m_action)
            m_action->setText(qstring_from_ak_string(action.text()));
    }

    virtual void on_tooltip_changed(WebView::Action& action) override
    {
        if (m_action)
            m_action->setToolTip(qstring_from_ak_string(action.tooltip()));
    }

    virtual void on_enabled_state_changed(WebView::Action& action) override
    {
        if (m_action)
            m_action->setEnabled(action.enabled());
    }

    virtual void on_visible_state_changed(WebView::Action& action) override
    {
        if (m_action) {
            m_action->setVisible(action.visible());

            for (auto* object : m_action->associatedObjects()) {
                if (auto* tool_button = as_if<QToolButton>(object))
                    tool_button->setVisible(action.visible());
            }
        }
    }

    virtual void on_engaged_state_changed(WebView::Action& action) override
    {
        if (m_include_action_icon == IncludeActionIcon::No)
            return;

        if (!m_action)
            return;

        switch (action.id()) {
        case WebView::ActionID::ToggleVerticalTabsExpanded:
            if (auto* parent = as_if<QWidget>(m_action->parent())) {
                auto const& tab_settings = WebView::Application::settings().tab_settings();
                auto icon = tab_settings.vertical_tabs_position == WebView::VerticalTabsPosition::Right
                    ? (action.engaged() ? ChromeIcon::VerticalTabBarCollapseRight : ChromeIcon::VerticalTabBarExpandRight)
                    : (action.engaged() ? ChromeIcon::VerticalTabBarCollapse : ChromeIcon::VerticalTabBarExpand);
                m_action->setIcon(create_chrome_icon(icon, parent->palette()));
            }
            break;

        case WebView::ActionID::ToggleBookmark:
        case WebView::ActionID::ToggleBookmarkViaToolbar:
            if (auto* parent = as_if<QWidget>(m_action->parent())) {
                auto icon = action.engaged() ? ChromeIcon::StarFilled : ChromeIcon::Star;
                m_action->setIcon(create_chrome_icon(icon, parent->palette()));
            }
            break;

        default:
            break;
        }
    }

    virtual void on_checked_state_changed(WebView::Action& action) override
    {
        if (m_action)
            m_action->setChecked(action.checked());
    }

private:
    ActionObserver(WebView::Action& action, QAction& qaction, IncludeActionIcon include_action_icon)
        : m_action(&qaction)
        , m_include_action_icon(include_action_icon)
    {
        QObject::connect(m_action, &QAction::triggered, [weak_action = action.make_weak_ptr()](bool checked) {
            if (auto action = weak_action.strong_ref()) {
                if (action->is_checkable())
                    action->set_checked(checked);
                action->activate();
            }
        });
        QObject::connect(m_action->parent(), &QObject::destroyed, [this, weak_action = action.make_weak_ptr()]() {
            if (auto action = weak_action.strong_ref())
                action->remove_observer(*this);
        });
    }

    QPointer<QAction> m_action;
    IncludeActionIcon m_include_action_icon { IncludeActionIcon::Yes };
};

class MenuObserver final : public WebView::Menu::Observer {
public:
    static NonnullOwnPtr<MenuObserver> create(QMenu& qmenu)
    {
        return adopt_own(*new MenuObserver(qmenu));
    }

    virtual void on_visible_state_changed(WebView::Menu& menu) override
    {
        if (m_menu && m_menu->menuAction())
            m_menu->menuAction()->setVisible(menu.visible());
    }

private:
    explicit MenuObserver(QMenu& qmenu)
        : m_menu(&qmenu)
    {
    }

    QPointer<QMenu> m_menu;
};

template<typename T>
static void add_properties(QObject& object, T& menu_or_action)
{
    for (auto const& [key, value] : menu_or_action.properties())
        object.setProperty(key.to_byte_string().characters(), qstring_from_ak_string(value));
}

static void initialize_native_control(WebView::Action& action, QAction& qaction, QPalette const& palette, IncludeActionIcon include_action_icon)
{
    static constexpr int const MENU_ICON_SIZE = 16;

    switch (action.id()) {
    case WebView::ActionID::NavigateBack:
        if (include_action_icon == IncludeActionIcon::Yes)
            qaction.setIcon(create_chrome_icon(ChromeIcon::Back, palette));
        qaction.setShortcuts(QKeySequence::keyBindings(QKeySequence::StandardKey::Back));
        break;
    case WebView::ActionID::NavigateForward:
        if (include_action_icon == IncludeActionIcon::Yes)
            qaction.setIcon(create_chrome_icon(ChromeIcon::Forward, palette));
        qaction.setShortcuts(QKeySequence::keyBindings(QKeySequence::StandardKey::Forward));
        break;
    case WebView::ActionID::Reload:
        if (include_action_icon == IncludeActionIcon::Yes)
            qaction.setIcon(create_chrome_icon(ChromeIcon::Reload, palette));
        qaction.setShortcuts({ QKeySequence(Qt::CTRL | Qt::Key_R), QKeySequence(Qt::Key_F5) });
        break;
    case WebView::ActionID::ViewDownloads:
        if (include_action_icon == IncludeActionIcon::Yes)
            qaction.setIcon(create_chrome_icon(ChromeIcon::Download, palette));
        qaction.setShortcut(QKeySequence(Qt::CTRL | Qt::Key_J));
        break;

    case WebView::ActionID::CopySelection:
        qaction.setShortcut(QKeySequence::StandardKey::Copy);
        break;
    case WebView::ActionID::CutSelection:
        qaction.setShortcut(QKeySequence::StandardKey::Cut);
        break;
    case WebView::ActionID::Paste:
        qaction.setShortcut(QKeySequence::StandardKey::Paste);
        break;
    case WebView::ActionID::SelectAll:
        qaction.setShortcut(QKeySequence::StandardKey::SelectAll);
        break;

    case WebView::ActionID::ToggleBookmark:
        if (include_action_icon == IncludeActionIcon::Yes)
            qaction.setIcon(create_chrome_icon(action.engaged() ? ChromeIcon::StarFilled : ChromeIcon::Star, palette));
        qaction.setShortcut(QKeySequence(Qt::CTRL | Qt::Key_D));
        break;
    case WebView::ActionID::ToggleBookmarkViaToolbar:
        if (include_action_icon == IncludeActionIcon::Yes)
            qaction.setIcon(create_chrome_icon(action.engaged() ? ChromeIcon::StarFilled : ChromeIcon::Star, palette));
        break;
    case WebView::ActionID::AddBookmarkAllTabs:
        qaction.setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_D));
        break;
    case WebView::ActionID::ToggleBookmarksBar:
        qaction.setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_B));
        break;
    case WebView::ActionID::BookmarkItem:
        if (auto icon = action.base64_png_icon(); icon.has_value())
            qaction.setIcon(icon_from_base64_png(*icon, MENU_ICON_SIZE));
        else
            qaction.setIcon(create_chrome_icon(ChromeIcon::Globe, palette));
        break;

    case WebView::ActionID::ViewHistory:
#if defined(AK_OS_MACOS)
        qaction.setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Y));
#else
        qaction.setShortcut(QKeySequence(Qt::CTRL | Qt::Key_H));
#endif
        break;
    case WebView::ActionID::ClearBrowsingData:
#if defined(AK_OS_MACOS)
        qaction.setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Backspace));
#else
        qaction.setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Delete));
#endif
        break;
    case WebView::ActionID::OpenProcessesPage:
        qaction.setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_M));
        break;
    case WebView::ActionID::OpenSettingsPage:
#if defined(AK_OS_MACOS)
        qaction.setShortcut(QKeySequence::StandardKey::Preferences);
#else
        qaction.setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Comma));
#endif
        break;
    case WebView::ActionID::ToggleDevTools:
        qaction.setShortcuts({
            QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_I),
            QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C),
            QKeySequence(Qt::Key_F12),
        });
        break;
    case WebView::ActionID::ViewSource:
        qaction.setShortcut(QKeySequence(Qt::CTRL | Qt::Key_U));
        break;

    case WebView::ActionID::ZoomIn: {
        auto zoom_in_shortcuts = QKeySequence::keyBindings(QKeySequence::StandardKey::ZoomIn);
        auto secondary_zoom_in_shortcut = QKeySequence(Qt::CTRL | Qt::Key_Equal);

        if (!zoom_in_shortcuts.contains(secondary_zoom_in_shortcut))
            zoom_in_shortcuts.append(move(secondary_zoom_in_shortcut));

        qaction.setShortcuts(zoom_in_shortcuts);
        break;
    }
    case WebView::ActionID::ZoomOut:
        qaction.setShortcut(QKeySequence::StandardKey::ZoomOut);
        break;
    case WebView::ActionID::ResetZoom:
        qaction.setShortcut(QKeySequence(Qt::CTRL | Qt::Key_0));
        break;

    default:
        break;
    }

    if (action.is_checkable())
        qaction.setCheckable(true);

    action.add_observer(ActionObserver::create(action, qaction, include_action_icon));
    add_properties(qaction, action);
}

static void add_items_to_menu(QMenu& qmenu, QWidget& parent, WebView::Menu& menu)
{
    menu.add_observer(MenuObserver::create(qmenu));
    add_properties(qmenu, menu);

    for (auto& menu_item : menu.items()) {
        menu_item.visit(
            [&](NonnullRefPtr<WebView::Action>& action) {
                auto* qaction = create_application_action(parent, action, IncludeActionIcon::No);
                qmenu.addAction(qaction);
            },
            [&](NonnullRefPtr<WebView::Menu> const& submenu) {
                auto* qsubmenu = new QMenu(qstring_from_ak_string(submenu->title()), &qmenu);
                add_items_to_menu(*qsubmenu, parent, submenu);

                if (submenu->render_group_icon())
                    qsubmenu->setIcon(create_chrome_icon(ChromeIcon::Folder, parent.palette()));

                add_properties(*qsubmenu, *submenu);
                qmenu.addMenu(qsubmenu);
            },
            [&](WebView::Separator) {
                qmenu.addSeparator();
            });
    }
}

static QAction* create_session_history_traversal_menu_action(QMenu& menu, WebContentView& view, WebView::ViewImplementation::SessionHistoryTraversalMenuItem const& item)
{
    static constexpr int const MENU_ICON_SIZE = 16;

    auto* action = new QAction(qstring_from_ak_string(item.title), &menu);
    action->setToolTip(qstring_from_ak_string(item.url));
    if (item.favicon_base64_png.has_value())
        action->setIcon(icon_from_base64_png(*item.favicon_base64_png, MENU_ICON_SIZE));
    else
        action->setIcon(create_chrome_icon(ChromeIcon::Globe, menu.palette()));
    QObject::connect(action, &QAction::triggered, &view, [&view, delta = item.delta] {
        (void)view.traverse_the_history_by_delta(delta);
    });
    return action;
}

static bool append_session_history_traversal_menu_items(QMenu& menu, WebContentView& view, int direction)
{
    auto items = view.session_history_traversal_menu_items(direction);
    if (items.is_empty())
        return false;

    for (auto const& item : items)
        menu.addAction(create_session_history_traversal_menu_action(menu, view, item));

    return true;
}

void populate_session_history_traversal_menu(QMenu& menu, WebContentView& view, int direction)
{
    menu.clear();
    append_session_history_traversal_menu_items(menu, view, direction);
}

QMenu* create_application_menu(QWidget& parent, WebView::Menu& menu)
{
    auto* application_menu = new QMenu(qstring_from_ak_string(menu.title()), &parent);
    add_items_to_menu(*application_menu, parent, menu);
    return application_menu;
}

void repopulate_application_menu(QMenu& menu, QWidget& parent, WebView::Menu& source)
{
    menu.clear();
    add_items_to_menu(menu, parent, source);
}

static void insert_dynamic_history_action(QMenu& menu, QAction* before, QAction& action)
{
    action.setProperty(DYNAMIC_HISTORY_MENU_ITEM_PROPERTY, true);
    menu.insertAction(before, &action);
}

static QAction* create_dynamic_history_separator(QMenu& menu)
{
    auto* separator = new QAction(&menu);
    separator->setSeparator(true);
    return separator;
}

static QAction* create_history_navigation_action(QMenu& menu, WebContentView& view, WebView::Action& source_action, QKeySequence::StandardKey shortcut)
{
    auto* action = new QAction(qstring_from_ak_string(source_action.text()), &menu);
    action->setEnabled(source_action.enabled());
    action->setShortcuts(QKeySequence::keyBindings(shortcut));
    QObject::connect(action, &QAction::triggered, &view, [&source_action] {
        source_action.activate();
    });
    return action;
}

static QAction* create_recent_history_menu_action(QMenu& menu, WebContentView& view, WebView::HistoryEntry const& entry)
{
    static constexpr int const MENU_ICON_SIZE = 16;

    auto title = entry.title.has_value() && !entry.title->is_empty() ? *entry.title : entry.url;
    auto* action = new QAction(qstring_from_ak_string(title), &menu);
    action->setToolTip(qstring_from_ak_string(entry.url));
    if (entry.favicon_base64_png.has_value())
        action->setIcon(icon_from_base64_png(*entry.favicon_base64_png, MENU_ICON_SIZE));
    else
        action->setIcon(create_chrome_icon(ChromeIcon::Globe, menu.palette()));

    auto url = URL::Parser::basic_parse(entry.url);
    if (url.has_value()) {
        QObject::connect(action, &QAction::triggered, &view, [&view, url = url.release_value()] {
            view.load(url);
        });
    } else {
        action->setEnabled(false);
    }

    return action;
}

void update_history_menu(QMenu& menu, WebContentView* view)
{
    for (auto* action : menu.actions()) {
        if (action->property(DYNAMIC_HISTORY_MENU_ITEM_PROPERTY).toBool()) {
            menu.removeAction(action);
            action->deleteLater();
        }
    }

    if (!view)
        return;

    auto* insertion_point = menu.actions().isEmpty() ? nullptr : menu.actions().first();
    insert_dynamic_history_action(menu, insertion_point, *create_history_navigation_action(menu, *view, view->navigate_back_action(), QKeySequence::StandardKey::Back));
    insert_dynamic_history_action(menu, insertion_point, *create_history_navigation_action(menu, *view, view->navigate_forward_action(), QKeySequence::StandardKey::Forward));
    insert_dynamic_history_action(menu, insertion_point, *create_dynamic_history_separator(menu));

    auto entries = WebView::Application::history_store(view->is_private()).list_entries({}, 0, RECENT_HISTORY_MENU_ITEM_LIMIT);
    for (auto const& entry : entries) {
        auto* action = create_recent_history_menu_action(menu, *view, entry);
        insert_dynamic_history_action(menu, insertion_point, *action);
    }

    if (!entries.is_empty())
        insert_dynamic_history_action(menu, insertion_point, *create_dynamic_history_separator(menu));
}

QMenu* create_context_menu(QWidget& parent, WebContentView& view, WebView::Menu& menu)
{
    auto* application_menu = create_application_menu(parent, menu);

    menu.on_activation = [view = QPointer { &view }, application_menu = QPointer { application_menu }](Gfx::IntPoint position) {
        if (view && application_menu)
            application_menu->exec(view->map_point_to_global_position(position));
    };

    return application_menu;
}

QAction* create_application_action(QWidget& parent, WebView::Action& action, IncludeActionIcon include_action_icon)
{
    auto* qaction = new QAction(&parent);
    initialize_native_control(action, *qaction, parent.palette(), include_action_icon);
    return qaction;
}

}
