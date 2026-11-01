/*
 * Copyright (c) 2025-2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Platform.h>
#include <LibWebView/Menu.h>

#include <QPointer>

class QAction;
class QMenu;
class QPoint;
class QWidget;

namespace Ladybird {

class WebContentView;

enum class IncludeActionIcon : u8 {
    No,
    Yes,
};

QMenu* create_application_menu(QWidget& parent, WebView::Menu&);
void repopulate_application_menu(QMenu& menu, WebView::Menu& source);
void update_history_menu(QMenu& menu, WebContentView*);
void populate_session_history_traversal_menu(QMenu& menu, WebContentView&, int direction);

QAction* create_application_action(QWidget& parent, WebView::Action&, IncludeActionIcon = IncludeActionIcon::Yes);

// A QAction that shows one WebView::Action at a time, such as the toolbar's for whichever tab is selected.
class ActionBinding {
public:
    explicit ActionBinding(QWidget& parent, IncludeActionIcon = IncludeActionIcon::Yes);

    QAction& action() const { return *m_action; }
    void bind(WebView::Action&);

private:
    QAction* m_action { nullptr };
    IncludeActionIcon m_include_action_icon { IncludeActionIcon::Yes };
    WeakPtr<WebView::Action> m_bound_action;
    WebView::Action::Observer* m_observer { nullptr };
};

#if defined(AK_OS_MACOS)
void enable_menu_icons(QMenu&);
#endif

void execute_context_menu(QMenu&, QPoint const& global_position);
QMenu& replace_context_menu(QPointer<QMenu>& shown_menu, QWidget& parent, WebView::Menu&);
void execute_context_menu_later(QPointer<QMenu>& shown_menu, QWidget& parent, WebView::Menu&, QPoint const& global_position);

}
