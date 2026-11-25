/*
 * Copyright (c) 2025-2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibURL/Parser.h>
#include <LibWebView/Application.h>
#include <LibWebView/HistoryStore.h>
#include <UI/Qt/Application.h>
#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/Icon.h>
#include <UI/Qt/Menu.h>
#include <UI/Qt/StringUtils.h>
#include <UI/Qt/Tab.h>
#include <UI/Qt/Theme.h>
#include <UI/Qt/WebContentView.h>

#include <QAction>
#include <QCryptographicHash>
#include <QHBoxLayout>
#include <QHash>
#include <QLabel>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QTimer>
#include <QToolButton>
#include <QWidget>

namespace Ladybird {

static constexpr auto DYNAMIC_HISTORY_MENU_ITEM_PROPERTY = "LadybirdDynamicHistoryMenuItem";
static constexpr auto HISTORY_NAVIGATION_SEPARATOR_PROPERTY = "LadybirdHistoryNavigationSeparator";
static constexpr size_t RECENT_HISTORY_MENU_ITEM_LIMIT = 15;

static void set_action_shortcuts(QAction&, ActionID);

class ActionObserver final : public Action::Observer {
public:
    static NonnullOwnPtr<ActionObserver> create(Action& action, QAction& qaction, IncludeActionIcon include_action_icon)
    {
        return adopt_own(*new ActionObserver(action, qaction, include_action_icon));
    }

    virtual ~ActionObserver() override
    {
        QObject::disconnect(m_triggered_connection);
        QObject::disconnect(m_destroyed_connection);
    }

    virtual void on_text_changed(Action& action) override
    {
        if (m_action)
            m_action->setText(qstring_from_ak_string(action.text()));
    }

    virtual void on_tooltip_changed(Action& action) override
    {
        if (m_action)
            m_action->setToolTip(qstring_from_ak_string(action.tooltip()));
    }

    virtual void on_enabled_state_changed(Action& action) override
    {
        if (m_action)
            m_action->setEnabled(action.enabled());
    }

    virtual void on_visible_state_changed(Action& action) override
    {
        if (m_action) {
            m_action->setVisible(action.visible());

            for (auto* object : m_action->associatedObjects()) {
                if (auto* tool_button = as_if<QToolButton>(object))
                    tool_button->setVisible(action.visible());
            }
        }
    }

    virtual void on_engaged_state_changed(Action& action) override
    {
        if (m_include_action_icon == IncludeActionIcon::No)
            return;

        if (!m_action)
            return;

        switch (action.id()) {
        case ActionID::ToggleVerticalTabsExpanded:
            if (auto* parent = as_if<QWidget>(m_action->parent())) {
                auto const& tab_settings = Application::settings().tab_settings();
                auto icon = tab_settings.vertical_tabs_position == WebView::VerticalTabsPosition::Right ? ChromeIcon::SidebarRight : ChromeIcon::Sidebar;
                m_action->setIcon(create_chrome_icon(icon, parent->palette()));
            }
            break;

        case ActionID::ToggleBookmark:
        case ActionID::ToggleBookmarkViaToolbar:
            // A bookmarked page's star is filled in.
            if (auto* parent = as_if<QWidget>(m_action->parent())) {
                auto icon = action.engaged() ? ChromeIcon::StarFilled : ChromeIcon::Star;
                m_action->setIcon(create_chrome_icon(icon, parent->palette()));
            }
            break;

        case ActionID::TranslatePage:
            if (auto* parent = as_if<QWidget>(m_action->parent()))
                m_action->setIcon(create_translate_icon(parent->palette(), action.engaged()));
            break;

        default:
            break;
        }
    }

    virtual void on_checked_state_changed(Action& action) override
    {
        if (m_action)
            m_action->setChecked(action.checked());
    }

    // QAction has no busy state, so it's a property, and the buttons that show it learn of a change as of any other.
    virtual void on_busy_state_changed(Action& action) override
    {
        if (!m_action || m_action->property(BUSY_ACTION_PROPERTY).toBool() == action.busy())
            return;
        m_action->setProperty(BUSY_ACTION_PROPERTY, action.busy());
        Q_EMIT m_action->changed();
    }

    virtual void on_shortcut_hint_changed(Action& action) override
    {
        if (!m_action)
            return;

        auto hint = action.shortcut_hint();
        set_action_shortcuts(*m_action, hint.value_or(action.id()));

        // A hint leaves the window shortcut with the action that owns it.
        m_action->setShortcutContext(hint.has_value() ? Qt::WidgetShortcut : Qt::WindowShortcut);
    }

private:
    ActionObserver(Action& action, QAction& qaction, IncludeActionIcon include_action_icon)
        : m_action(&qaction)
        , m_include_action_icon(include_action_icon)
    {
        m_triggered_connection = QObject::connect(m_action, &QAction::triggered, [weak_action = action.make_weak_ptr()](bool checked) {
            if (auto action = weak_action.strong_ref()) {
                if (action->is_checkable())
                    action->set_checked(checked);
                action->activate();

                if (first_is_one_of(action->id(), ActionID::BookmarkItem, ActionID::Reload, ActionID::ReloadBypassingCache)) {
                    if (auto* active_tab = Application::the().active_tab())
                        active_tab->view().setFocus();
                }
            }
        });
        m_destroyed_connection = QObject::connect(&qaction, &QObject::destroyed, [this, weak_action = action.make_weak_ptr()]() {
            if (auto action = weak_action.strong_ref())
                action->remove_observer(*this);
        });
    }

    QPointer<QAction> m_action;
    IncludeActionIcon m_include_action_icon { IncludeActionIcon::Yes };
    QMetaObject::Connection m_triggered_connection;
    QMetaObject::Connection m_destroyed_connection;
};

class MenuObserver final : public Menu::Observer {
public:
    static NonnullOwnPtr<MenuObserver> create(Menu& menu, QMenu& qmenu)
    {
        return adopt_own(*new MenuObserver(menu, qmenu));
    }

    virtual void on_visible_state_changed(Menu& menu) override
    {
        if (m_menu && m_menu->menuAction())
            m_menu->menuAction()->setVisible(menu.visible());
    }

private:
    MenuObserver(Menu& menu, QMenu& qmenu)
        : m_menu(&qmenu)
    {
        QObject::connect(&qmenu, &QObject::destroyed, [this, weak_menu = menu.make_weak_ptr()]() {
            if (auto menu = weak_menu.strong_ref())
                menu->remove_observer(*this);
        });
    }

    QPointer<QMenu> m_menu;
};

template<typename T>
static void add_properties(QObject& object, T& menu_or_action)
{
    for (auto const& [key, value] : menu_or_action.properties())
        object.setProperty(key.to_byte_string().characters(), qstring_from_ak_string(value));
}

static void set_action_shortcuts(QAction& qaction, ActionID id)
{
    qaction.setShortcuts({});

    switch (id) {
    case ActionID::NavigateBack:
        qaction.setShortcuts(QKeySequence::keyBindings(QKeySequence::StandardKey::Back));
        break;
    case ActionID::NavigateForward:
        qaction.setShortcuts(QKeySequence::keyBindings(QKeySequence::StandardKey::Forward));
        break;
    case ActionID::Reload:
        qaction.setShortcuts({ QKeySequence(Qt::CTRL | Qt::Key_R), QKeySequence(Qt::Key_F5) });
        break;
    case ActionID::ReloadBypassingCache:
#if defined(AK_OS_MACOS)
        qaction.setShortcuts({ QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_R), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_R) });
#else
        qaction.setShortcuts({ QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_R), QKeySequence(Qt::CTRL | Qt::Key_F5) });
#endif
        break;
    case ActionID::ViewDownloads:
#if defined(AK_OS_MACOS)
        qaction.setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_L));
#else
        qaction.setShortcut(QKeySequence(Qt::CTRL | Qt::Key_J));
#endif
        break;

    case ActionID::Undo:
        qaction.setShortcut(QKeySequence::StandardKey::Undo);
        break;
    case ActionID::Redo:
        qaction.setShortcuts(QKeySequence::keyBindings(QKeySequence::StandardKey::Redo));
        break;
    case ActionID::CopySelection:
        qaction.setShortcut(QKeySequence::StandardKey::Copy);
        break;
    case ActionID::CutSelection:
        qaction.setShortcut(QKeySequence::StandardKey::Cut);
        break;
    case ActionID::Paste:
        qaction.setShortcut(QKeySequence::StandardKey::Paste);
        break;
    case ActionID::PasteAndMatchStyle:
#if defined(AK_OS_MACOS)
        qaction.setShortcuts({ QKeySequence(Qt::CTRL | Qt::ALT | Qt::SHIFT | Qt::Key_V), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_V) });
#else
        qaction.setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_V));
#endif
        break;
    case ActionID::SelectAll:
        qaction.setShortcut(QKeySequence::StandardKey::SelectAll);
        break;

    case ActionID::ToggleBookmark:
        qaction.setShortcut(QKeySequence(Qt::CTRL | Qt::Key_D));
        break;
    case ActionID::ToggleBookmarksBar:
        qaction.setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_B));
        break;
    case ActionID::ToggleVerticalTabsExpanded:
#if defined(AK_OS_MACOS)
        qaction.setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_S));
#else
        qaction.setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S));
#endif
        break;
    case ActionID::AddBookmarkAllTabs:
        qaction.setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_D));
        break;
    case ActionID::ViewHistory:
#if defined(AK_OS_MACOS)
        qaction.setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Y));
#else
        qaction.setShortcut(QKeySequence(Qt::CTRL | Qt::Key_H));
#endif
        break;
    case ActionID::ManageBookmarks:
#if defined(AK_OS_MACOS)
        qaction.setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_B));
#else
        qaction.setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O));
#endif
        break;
    case ActionID::ClearBrowsingData:
#if defined(AK_OS_MACOS)
        qaction.setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Backspace));
#else
        qaction.setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Delete));
#endif
        break;
    case ActionID::OpenTaskManager:
        qaction.setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_M));
        break;
    case ActionID::OpenSettingsPage:
#if defined(AK_OS_MACOS)
        qaction.setShortcut(QKeySequence::StandardKey::Preferences);
#else
        qaction.setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Comma));
#endif
        break;
    case ActionID::ToggleDevTools:
        qaction.setShortcuts({
            QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_I),
            QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C),
            QKeySequence(Qt::Key_F12),
        });
        break;
    case ActionID::ViewSource:
#if defined(AK_OS_MACOS)
        qaction.setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_U));
#else
        qaction.setShortcut(QKeySequence(Qt::CTRL | Qt::Key_U));
#endif
        break;

    case ActionID::ZoomIn: {
        auto zoom_in_shortcuts = QKeySequence::keyBindings(QKeySequence::StandardKey::ZoomIn);
        auto secondary_zoom_in_shortcut = QKeySequence(Qt::CTRL | Qt::Key_Equal);

        if (!zoom_in_shortcuts.contains(secondary_zoom_in_shortcut))
            zoom_in_shortcuts.append(move(secondary_zoom_in_shortcut));

        qaction.setShortcuts(zoom_in_shortcuts);
        break;
    }
    case ActionID::ZoomOut:
        qaction.setShortcut(QKeySequence::StandardKey::ZoomOut);
        break;
    case ActionID::ResetZoom:
        qaction.setShortcut(QKeySequence(Qt::CTRL | Qt::Key_0));
        break;

    default:
        break;
    }
}

static Action::Observer& initialize_native_control(Action& action, QAction& qaction, QPalette const& palette, IncludeActionIcon include_action_icon)
{
    static constexpr int const MENU_ICON_SIZE = 16;

    switch (action.id()) {
    case ActionID::NavigateBack:
        if (include_action_icon == IncludeActionIcon::Yes)
            qaction.setIcon(create_chrome_icon(ChromeIcon::Back, palette));
        break;
    case ActionID::NavigateForward:
        if (include_action_icon == IncludeActionIcon::Yes)
            qaction.setIcon(create_chrome_icon(ChromeIcon::Forward, palette));
        break;
    case ActionID::Reload:
        if (include_action_icon == IncludeActionIcon::Yes)
            qaction.setIcon(create_chrome_icon(ChromeIcon::Reload, palette));
        break;
    case ActionID::ViewDownloads:
        if (include_action_icon == IncludeActionIcon::Yes)
            qaction.setIcon(create_chrome_icon(ChromeIcon::Download, palette));
        break;
    case ActionID::ToggleBookmark:
    case ActionID::ToggleBookmarkViaToolbar:
        if (include_action_icon == IncludeActionIcon::Yes)
            qaction.setIcon(create_chrome_icon(action.engaged() ? ChromeIcon::StarFilled : ChromeIcon::Star, palette));
        break;
    case ActionID::TranslatePage:
        if (include_action_icon == IncludeActionIcon::Yes)
            qaction.setIcon(create_translate_icon(palette, action.engaged()));
        break;
    case ActionID::BookmarkItem:
        if (auto icon = action.png_icon(); icon.has_value())
            qaction.setIcon(favicon_from_png(icon->bytes(), MENU_ICON_SIZE));
        else
            qaction.setIcon(create_chrome_icon(ChromeIcon::Globe, palette));
        break;
    default:
        break;
    }

    qaction.setShortcutVisibleInContextMenu(true);

    if (action.is_checkable())
        qaction.setCheckable(true);

    auto observer = ActionObserver::create(action, qaction, include_action_icon);
    auto& observer_reference = *observer;
    action.add_observer(move(observer));
    add_properties(qaction, action);
    return observer_reference;
}

static void add_items_to_menu(QMenu& qmenu, Menu& menu)
{
    for (auto& menu_item : menu.items()) {
        menu_item.visit(
            [&](NonnullRefPtr<Action>& action) {
                auto* qaction = create_application_action(qmenu, action, IncludeActionIcon::No);
                qmenu.addAction(qaction);
            },
            [&](NonnullRefPtr<Menu> const& submenu) {
                auto* qsubmenu = create_application_menu(qmenu, submenu);

                if (submenu->render_group_icon())
                    qsubmenu->setIcon(create_chrome_icon(ChromeIcon::Folder, qmenu.palette()));

                qmenu.addMenu(qsubmenu);
            },
            [&](Separator) {
                qmenu.addSeparator();
            });
    }

#if defined(AK_OS_MACOS)
    enable_menu_icons(qmenu);
#endif
}

static QAction* create_session_history_traversal_menu_action(QMenu& menu, RemoteView::SessionHistoryTraversalMenuItem const& item)
{
    static constexpr int const MENU_ICON_SIZE = 16;

    auto* action = new QAction(qstring_from_ak_string(item.title), &menu);
    action->setToolTip(qstring_from_ak_string(item.url));
    if (item.favicon_png.has_value())
        action->setIcon(favicon_from_png(item.favicon_png->bytes(), MENU_ICON_SIZE));
    else
        action->setIcon(create_chrome_icon(ChromeIcon::Globe, menu.palette()));
    action->setData(item.step);
    return action;
}

static bool append_session_history_traversal_menu_items(QMenu& menu, WebContentView& view, int direction)
{
    auto items = view.session_history_traversal_menu_items(direction);
    if (items.is_empty())
        return false;

    for (auto const& item : items)
        menu.addAction(create_session_history_traversal_menu_action(menu, item));

#if defined(AK_OS_MACOS)
    enable_menu_icons(menu);
#endif

    return true;
}

void populate_session_history_traversal_menu(QMenu& menu, WebContentView& view, int direction)
{
    menu.clear();
    append_session_history_traversal_menu_items(menu, view, direction);
}

QMenu* create_application_menu(QWidget& parent, Menu& menu)
{
    auto* application_menu = new QMenu(qstring_from_ak_string(menu.title()), &parent);
    menu.add_observer(MenuObserver::create(menu, *application_menu));
    add_properties(*application_menu, menu);
    add_items_to_menu(*application_menu, menu);
    return application_menu;
}

// NB: Qt deletes the native items of a menu's actions when they are removed from it, but not when the menu is
//     destroyed. So the actions are removed from the menu and its submenus first.
static void remove_actions(QMenu& menu)
{
    for (auto* action : menu.actions()) {
        menu.removeAction(action);
        if (auto* submenu = action->menu())
            remove_actions(*submenu);
    }
}

void delete_menu_later(QMenu& menu)
{
    remove_actions(menu);
    menu.deleteLater();
}

QMenu& replace_context_menu(QPointer<QMenu>& shown_menu, QWidget& parent, Menu& menu)
{
    // NB: A menu is kept until another is shown in its place, as Cocoa delivers a native menu's choice through queued
    //     events after the menu closes.
    if (shown_menu)
        delete_menu_later(*shown_menu);
    shown_menu = create_application_menu(parent, menu);
    return *shown_menu;
}

void execute_context_menu_later(QPointer<QMenu>& shown_menu, QWidget& parent, Menu& menu, QPoint const& global_position)
{
    auto& context_menu = replace_context_menu(shown_menu, parent, menu);

    // NB: Showing a menu runs a nested event loop, which may destroy whatever asked for the menu. So it runs with
    //     nothing of the caller's on the stack.
    QTimer::singleShot(0, &context_menu, [&context_menu, global_position] {
        execute_context_menu(context_menu, global_position);
    });
}

void repopulate_application_menu(QMenu& menu, Menu& source)
{
    // NB: A submenu's action belongs to the submenu, so clearing the menu only removes it.
    for (auto* action : menu.actions()) {
        if (auto* submenu = action->menu())
            delete_menu_later(*submenu);
    }
    menu.clear();
    add_items_to_menu(menu, source);
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

// The icon of a favicon in the History menu, decoded and scaled once per distinct PNG.
static QIcon history_menu_favicon(ReadonlyBytes favicon_png, int logical_size)
{
    static constexpr qsizetype MAX_CACHED_FAVICONS = 64;
    static QHash<QByteArray, QIcon> s_favicons;

    auto key = QCryptographicHash::hash(QByteArrayView(favicon_png.data(), static_cast<qsizetype>(favicon_png.size())), QCryptographicHash::Sha1);
    if (auto it = s_favicons.constFind(key); it != s_favicons.constEnd())
        return *it;
    if (s_favicons.size() >= MAX_CACHED_FAVICONS)
        s_favicons.clear();
    auto icon = favicon_from_png(favicon_png, logical_size);
    s_favicons.insert(key, icon);
    return icon;
}

// What the recent entries in a History menu were made from: the view they navigate, and a digest of the menu's palette
// and of each entry's URL, title and favicon. A menu whose entries would come out the same keeps the ones it has.
class HistoryMenuEntriesState final : public QObject {
public:
    AK_ALLOC_WITH_KMALLOC;

    explicit HistoryMenuEntriesState(QMenu& menu)
        : QObject(&menu)
    {
        setObjectName(QStringLiteral("LadybirdHistoryMenuEntriesState"));
    }

    QPointer<QObject> context;
    QByteArray digest;
    // The entries' actions navigate through whichever navigator the menu was last updated with.
    HistoryMenuNavigator navigate;
};

// Makes `action` show `entry` and navigate to it, whether the action is new or showed another entry before.
static void show_history_entry_in_action(QAction& action, QMenu& menu, QObject& context, HistoryMenuEntriesState& state, WebView::HistoryEntry const& entry)
{
    static constexpr int const MENU_ICON_SIZE = 16;

    auto title = entry.title.has_value() && !entry.title->is_empty() ? *entry.title : entry.url;
    action.setText(qstring_from_ak_string(title));
    action.setToolTip(qstring_from_ak_string(entry.url));
    if (entry.favicon_png.has_value())
        action.setIcon(history_menu_favicon(entry.favicon_png->bytes(), MENU_ICON_SIZE));
    else
        action.setIcon(create_chrome_icon(ChromeIcon::Globe, menu.palette()));

    QObject::disconnect(&action, &QAction::triggered, nullptr, nullptr);
    auto url = URL::Parser::basic_parse(entry.url);
    action.setEnabled(url.has_value());
    if (url.has_value()) {
        QObject::connect(&action, &QAction::triggered, &context, [&state, url = url.release_value()] {
            // A copy, since navigating can update the menu, and with it the navigator.
            auto navigate = state.navigate;
            if (navigate)
                navigate(url);
        });
    }
}

// NB: Back and Forward stay in the History menu at all times, so their shortcuts work before the menu is first shown.
void add_history_navigation_actions(QMenu& menu, QAction& back, QAction& forward)
{
    auto* first = menu.actions().isEmpty() ? nullptr : menu.actions().first();
    menu.insertAction(first, &back);
    menu.insertAction(first, &forward);
    menu.insertSeparator(first)->setProperty(HISTORY_NAVIGATION_SEPARATOR_PROPERTY, true);
}

static QByteArray digest_of_history_entries(Vector<WebView::HistoryEntry> const& entries, qint64 palette_cache_key)
{
    QCryptographicHash hash(QCryptographicHash::Sha1);
    hash.addData(QByteArrayView(reinterpret_cast<char const*>(&palette_cache_key), sizeof(palette_cache_key)));
    auto add_bytes = [&](ReadonlyBytes bytes) {
        auto size = static_cast<u64>(bytes.size());
        hash.addData(QByteArrayView(reinterpret_cast<char const*>(&size), sizeof(size)));
        hash.addData(QByteArrayView(bytes.data(), static_cast<qsizetype>(bytes.size())));
    };
    for (auto const& entry : entries) {
        add_bytes(entry.url.bytes());
        add_bytes(entry.title.has_value() ? entry.title->bytes() : ReadonlyBytes {});
        // An entry without a favicon shows a fallback icon, and one with an empty favicon shows none.
        u8 const has_favicon = entry.favicon_png.has_value();
        hash.addData(QByteArrayView(reinterpret_cast<char const*>(&has_favicon), sizeof(has_favicon)));
        add_bytes(entry.favicon_png.has_value() ? entry.favicon_png->bytes() : ReadonlyBytes {});
    }
    return hash.result();
}

void update_history_menu(QMenu& menu, WebContentView* view)
{
    if (!view) {
        update_history_menu(menu, nullptr, {}, {});
        return;
    }
    update_history_menu(menu, view, view->recent_history_entries(RECENT_HISTORY_MENU_ITEM_LIMIT), [view = QPointer<WebContentView>(view)](URL::URL const& url) {
        if (view)
            view->load_from_user_input(url);
    });
}

void update_history_menu(QMenu& menu, QObject* context, Vector<WebView::HistoryEntry> const& entries, HistoryMenuNavigator navigate)
{
    // A global menu over D-Bus can ask for the menu to be prepared again and again, and every change to the menu's
    // actions is announced back to it, under item IDs that each new action takes up for good. So the menu is left
    // alone when what it shows wouldn't change, and otherwise keeps its actions and shows other entries in them,
    // adding or removing actions only where the number of entries changes.
    // Only update_history_menu() makes a child of the menu with this name, so the child it finds is its own state.
    auto* state = static_cast<HistoryMenuEntriesState*>(menu.findChild<QObject*>(QStringLiteral("LadybirdHistoryMenuEntriesState"), Qt::FindDirectChildrenOnly));
    if (!state)
        state = new HistoryMenuEntriesState(menu);
    state->navigate = move(navigate);
    // The palette is part of what the menu shows: Entries without a favicon show an icon drawn in its colors.
    auto digest = digest_of_history_entries(entries, menu.palette().cacheKey());
    if (context && state->context == context && !state->digest.isEmpty() && state->digest == digest)
        return;
    state->context = context;
    state->digest = digest;

    Vector<QAction*> entry_actions;
    QAction* separator = nullptr;
    for (auto* action : menu.actions()) {
        if (!action->property(DYNAMIC_HISTORY_MENU_ITEM_PROPERTY).toBool())
            continue;
        if (action->isSeparator())
            separator = action;
        else
            entry_actions.append(action);
    }

    auto shown_entry_count = context ? entries.size() : 0;
    while (entry_actions.size() > shown_entry_count) {
        auto* action = entry_actions.take_last();
        menu.removeAction(action);
        action->deleteLater();
    }
    if (shown_entry_count == 0) {
        if (separator) {
            menu.removeAction(separator);
            separator->deleteLater();
        }
        return;
    }

    // NB: The recent entries go after Back, Forward and the separator below them.
    QAction* insertion_point = nullptr;
    auto actions = menu.actions();
    for (qsizetype i = 0; i < actions.size(); ++i) {
        if (actions[i]->property(HISTORY_NAVIGATION_SEPARATOR_PROPERTY).toBool()) {
            insertion_point = i + 1 < actions.size() ? actions[i + 1] : nullptr;
            break;
        }
    }
    if (!separator) {
        separator = create_dynamic_history_separator(menu);
        insert_dynamic_history_action(menu, insertion_point, *separator);
    }
    while (entry_actions.size() < shown_entry_count) {
        auto* action = new QAction(&menu);
        insert_dynamic_history_action(menu, separator, *action);
        entry_actions.append(action);
    }

    for (size_t i = 0; i < shown_entry_count; ++i)
        show_history_entry_in_action(*entry_actions[i], menu, *context, *state, entries[i]);

#if defined(AK_OS_MACOS)
    enable_menu_icons(menu);
#endif
}

QAction* create_application_action(QWidget& parent, Action& action, IncludeActionIcon include_action_icon)
{
    auto* qaction = new QAction(&parent);
    initialize_native_control(action, *qaction, parent.palette(), include_action_icon);
    return qaction;
}

// The menu's zoom row: its label like an item's, and the page's zoom level between buttons to zoom out and in.
MenuZoomRow::MenuZoomRow(QMenu& menu, QAction& zoom_out, QAction& reset_zoom, QAction& zoom_in, QAction& zoom_level)
    : QWidgetAction(&menu)
{
    auto* row = new QWidget(&menu);
    row->setObjectName("LadybirdMenuZoomRow");
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(10, 3, 10, 3);
    layout->setSpacing(2);
    layout->addWidget(new QLabel("Zoom", row), 1);

    auto add_button = [&](char const* object_name, QString const& tooltip, QAction& action) {
        auto* button = new QPushButton(row);
        button->setObjectName(object_name);
        button->setToolTip(tooltip);
        button->setAccessibleName(tooltip);
        button->setFocusPolicy(Qt::NoFocus);
        QObject::connect(button, &QPushButton::clicked, &action, &QAction::trigger);
        layout->addWidget(button);
        return button;
    };
    m_zoom_out = add_button("LadybirdMenuZoomButton", "Zoom Out", zoom_out);
    auto* level = add_button("LadybirdMenuZoomLevel", "Reset Zoom", reset_zoom);
    // NB: The level is drawn in figures of one width, so it doesn't shift as it changes.
    auto level_font = level->font();
    level_font.setFeature(QFont::Tag("tnum"), 1);
    level->setFont(level_font);
    m_zoom_in = add_button("LadybirdMenuZoomButton", "Zoom In", zoom_in);

    auto update_level = [&zoom_level, level] {
        // The address bar hides its zoom action at the default level and keeps its last text.
        level->setText(zoom_level.isVisible() && !zoom_level.text().isEmpty() ? zoom_level.text() : QStringLiteral("100%"));
    };
    QObject::connect(&zoom_level, &QAction::changed, level, update_level);
    update_level();

    setDefaultWidget(row);
    update_style();
}

void MenuZoomRow::update_style()
{
    defaultWidget()->setStyleSheet(ChromeStyle::menu_zoom_row_style_sheet());
    m_zoom_out->setIcon(create_chrome_icon(ChromeIcon::ZoomOut, Theme::panel().body));
    m_zoom_in->setIcon(create_chrome_icon(ChromeIcon::NewTab, Theme::panel().body));
}

ActionBinding::ActionBinding(QWidget& parent, IncludeActionIcon include_action_icon)
    : m_action(new QAction(&parent))
    , m_include_action_icon(include_action_icon)
{
}

void ActionBinding::bind(Action& action)
{
    if (m_bound_action.ptr() == &action)
        return;
    if (auto bound_action = m_bound_action.strong_ref())
        bound_action->remove_observer(*m_observer);

    m_bound_action = action;
    auto const& palette = static_cast<QWidget*>(m_action->parent())->palette();
    m_observer = &initialize_native_control(action, *m_action, palette, m_include_action_icon);
}

#if !defined(AK_OS_MACOS)
void execute_context_menu(QMenu& menu, QPoint const& global_position)
{
    menu.exec(global_position);
}
#endif

}
