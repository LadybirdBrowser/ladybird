/*
 * Copyright (c) 2025-2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Platform.h>
#include <AK/kmalloc.h>
#include <UI/Qt/Actions.h>

#include <QPointer>
#include <QWidgetAction>

class QAction;
class QMenu;
class QPoint;
class QPushButton;
class QWidget;

namespace Ladybird {

class WebContentView;

enum class IncludeActionIcon : u8 {
    No,
    Yes,
};

QMenu* create_application_menu(QWidget& parent, Menu&);
void repopulate_application_menu(QMenu& menu, Menu& source);
void add_history_navigation_actions(QMenu&, QAction& back, QAction& forward);
void update_history_menu(QMenu& menu, WebContentView*);
void populate_session_history_traversal_menu(QMenu& menu, WebContentView&, int direction);

QAction* create_application_action(QWidget& parent, Action&, IncludeActionIcon = IncludeActionIcon::Yes);

class MenuZoomRow final : public QWidgetAction {
public:
    AK_ALLOC_WITH_KMALLOC;

    MenuZoomRow(QMenu&, QAction& zoom_out, QAction& reset_zoom, QAction& zoom_in, QAction& zoom_level);

    void update_style();

private:
    QPushButton* m_zoom_out { nullptr };
    QPushButton* m_zoom_in { nullptr };
};

// The property of a QAction that's true while its Action is busy.
static constexpr auto BUSY_ACTION_PROPERTY = "LadybirdBusyAction";

// A QAction that shows one Action at a time, such as the toolbar's for whichever tab is selected.
class ActionBinding {
public:
    explicit ActionBinding(QWidget& parent, IncludeActionIcon = IncludeActionIcon::Yes);

    QAction& action() const { return *m_action; }
    void bind(Action&);

private:
    QAction* m_action { nullptr };
    IncludeActionIcon m_include_action_icon { IncludeActionIcon::Yes };
    WeakPtr<Action> m_bound_action;
    Action::Observer* m_observer { nullptr };
};

#if defined(AK_OS_MACOS)
void enable_menu_icons(QMenu&);
#endif

void execute_context_menu(QMenu&, QPoint const& global_position);
QMenu& replace_context_menu(QPointer<QMenu>& shown_menu, QWidget& parent, Menu&);
void execute_context_menu_later(QPointer<QMenu>& shown_menu, QWidget& parent, Menu&, QPoint const& global_position);

}
