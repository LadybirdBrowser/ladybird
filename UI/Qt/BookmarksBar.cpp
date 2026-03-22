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
#include <QToolButton>

namespace Ladybird {

static constexpr int BOOKMARK_BUTTON_MAX_WIDTH = 150;
static constexpr int BOOKMARK_BUTTON_ICON_SIZE = 16;

BookmarksBar::BookmarksBar(QWidget* parent)
    : QToolBar(parent)
{
    setIconSize({ BOOKMARK_BUTTON_ICON_SIZE, BOOKMARK_BUTTON_ICON_SIZE });
    setVisible(WebView::Application::settings().show_bookmarks_bar());
    setMovable(false);

    rebuild();
}

void BookmarksBar::rebuild()
{
    clear();

    auto set_button_properties = [&](QToolButton* button, QString const& title) {
        button->setText(button->fontMetrics().elidedText(title, Qt::ElideRight, BOOKMARK_BUTTON_MAX_WIDTH - 28));
        button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        button->setMaximumWidth(BOOKMARK_BUTTON_MAX_WIDTH);
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

                auto* action = new QAction(title, this);
                action->setIcon(create_tvg_icon_with_theme_colors("folder", palette()));

                auto* submenu = create_application_menu(*this, *folder);
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

}
