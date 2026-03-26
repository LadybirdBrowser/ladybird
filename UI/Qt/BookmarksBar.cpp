/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/Application.h>
#include <UI/Qt/BookmarksBar.h>
#include <UI/Qt/Icon.h>
#include <UI/Qt/Menu.h>
#include <UI/Qt/StringUtils.h>

#include <QAction>
#include <QMenu>
#include <QMouseEvent>
#include <QToolButton>
#include <qapplication.h>

namespace Ladybird {

static constexpr int BOOKMARK_BUTTON_MAX_WIDTH = 150;
static constexpr int BOOKMARK_BUTTON_ICON_SIZE = 16;

static void install_menu_event_filter(QObject* filter, QMenu* menu)
{
    menu->installEventFilter(filter);

    for (auto* action : menu->actions()) {
        if (auto* submenu = action->menu())
            install_menu_event_filter(filter, submenu);
    }
}

static String extract_item_id(QObject* item)
{
    return ak_string_from_qstring(item->property("id").toString());
}

static Optional<String> extract_item_target_folder_id(QObject* item)
{
    if (auto value = ak_string_from_qstring(item->property("target_folder_id").toString()); !value.is_empty())
        return value;
    return {};
}

static QString extract_item_type(QObject* item)
{
    return item->property("type").toString();
}

BookmarksBar::BookmarksBar(QWidget* parent)
    : QToolBar(parent)
{
    setIconSize({ BOOKMARK_BUTTON_ICON_SIZE, BOOKMARK_BUTTON_ICON_SIZE });
    setVisible(WebView::Application::settings().show_bookmarks_bar());
    setMovable(false);

    installEventFilter(this);

    rebuild();
}

void BookmarksBar::rebuild()
{
    clear();

    auto set_button_properties = [&](QToolButton* button, QString const& title) {
        button->setText(button->fontMetrics().elidedText(title, Qt::ElideRight, BOOKMARK_BUTTON_MAX_WIDTH - 28));
        button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        button->setMaximumWidth(BOOKMARK_BUTTON_MAX_WIDTH);
        button->installEventFilter(this);
    };

    for (auto const& item : WebView::Application::the().bookmarks_menu().items()) {
        item.visit(
            [&](NonnullRefPtr<WebView::Action> const& bookmark) {
                if (bookmark->id() != WebView::ActionID::BookmarkItem)
                    return;

                auto* action = create_application_action(*this, *bookmark);
                addAction(action);

                if (auto* button = as_if<QToolButton>(widgetForAction(action)))
                    set_button_properties(button, qstring_from_ak_string(bookmark->text()));
            },
            [&](NonnullRefPtr<WebView::Menu> const& folder) {
                auto title = qstring_from_ak_string(folder->title());

                auto* submenu = create_application_menu(*this, *folder);
                install_menu_event_filter(this, submenu);

                auto* action = new QAction(title, this);
                action->setIcon(create_tvg_icon_with_theme_colors("folder", palette()));
                action->setProperty("id", submenu->property("id"));
                action->setProperty("type", submenu->property("type"));
                action->setProperty("target_folder_id", submenu->property("target_folder_id"));
                action->setMenu(submenu);
                addAction(action);

                if (auto* button = as_if<QToolButton>(widgetForAction(action))) {
                    button->setPopupMode(QToolButton::InstantPopup);
                    set_button_properties(button, title);
                }
            },
            [](WebView::Separator) {
            });
    }
}

bool BookmarksBar::eventFilter(QObject* object, QEvent* event)
{
    if (event->type() != QEvent::MouseButtonPress)
        return QToolBar::eventFilter(object, event);

    auto const& mouse_event = as<QMouseEvent>(*event);
    if (mouse_event.button() != Qt::RightButton)
        return QToolBar::eventFilter(object, event);

    if (is<BookmarksBar>(object)) {
        m_bookmark_context_menu_item_id = {};
        m_bookmark_context_menu_target_folder_id = {};

        bookmarks_bar_context_menu().exec(mouse_event.globalPosition().toPoint());
        return true;
    }

    QString type;

    auto extract_properties = [&](QObject* item) {
        m_bookmark_context_menu_item_id = extract_item_id(item);
        m_bookmark_context_menu_target_folder_id = extract_item_target_folder_id(item);
        type = extract_item_type(item);
    };

    if (auto* button = as_if<QToolButton>(object)) {
        auto* action = button->defaultAction();
        extract_properties(action);

        if (type == "bookmark")
            bookmark_context_menu().exec(mouse_event.globalPosition().toPoint());
        else if (type == "folder")
            bookmark_folder_context_menu().exec(mouse_event.globalPosition().toPoint());

        return true;
    }

    if (auto* menu = as_if<QMenu>(object)) {
        if (auto* action = menu->actionAt(mouse_event.pos())) {
            QObject* submenu = action->menu();
            extract_properties(submenu ?: action);
        }

        if (type.isEmpty())
            extract_properties(menu);

        // FIXME: We create a temporary context menu parented to the dropdown. Otherwise, Qt complains that the context
        //        menu's parent does not match the current topmost popup. It would be nice if we could figure out a way
        //        to avoid this duplicated menu.
        QMenu context_menu(menu);

        if (type == "bookmark")
            repopulate_application_menu(context_menu, context_menu, WebView::Application::the().bookmark_context_menu());
        else if (type == "folder")
            repopulate_application_menu(context_menu, context_menu, WebView::Application::the().bookmark_folder_context_menu());

        if (!context_menu.isEmpty() && context_menu.exec(mouse_event.globalPosition().toPoint()))
            menu->close();

        return true;
    }

    return QToolBar::eventFilter(object, event);
}

QMenu& BookmarksBar::bookmarks_bar_context_menu()
{
    if (!m_bookmarks_bar_context_menu)
        m_bookmarks_bar_context_menu = create_application_menu(*this, WebView::Application::the().bookmarks_bar_context_menu());
    return *m_bookmarks_bar_context_menu;
}

QMenu& BookmarksBar::bookmark_context_menu()
{
    if (!m_bookmark_context_menu)
        m_bookmark_context_menu = create_application_menu(*this, WebView::Application::the().bookmark_context_menu());
    return *m_bookmark_context_menu;
}

QMenu& BookmarksBar::bookmark_folder_context_menu()
{
    if (!m_bookmark_folder_context_menu)
        m_bookmark_folder_context_menu = create_application_menu(*this, WebView::Application::the().bookmark_folder_context_menu());
    return *m_bookmark_folder_context_menu;
}

}
