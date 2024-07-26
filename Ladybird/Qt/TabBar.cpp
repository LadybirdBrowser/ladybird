/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Tab.h"
#include <AK/StdLibExtras.h>
#include <AK/TypeCasts.h>
#include <Ladybird/Qt/TabBar.h>
#include <QContextMenuEvent>
#include <QEvent>
#include <QPushButton>
#include <QStylePainter>

namespace Ladybird {

TabBar::TabBar(QWidget* parent)
    : QTabBar(parent)
{
}

QSize TabBar::tabSizeHint(int index) const
{
    auto width = this->width() / count();
    width = min(225, width);
    width = max(128, width);

    auto hint = QTabBar::tabSizeHint(index);
    hint.setWidth(width);
    return hint;
}

void TabBar::contextMenuEvent(QContextMenuEvent* event)
{
    auto* tab_widget = verify_cast<QTabWidget>(this->parent());
    auto* tab = verify_cast<Tab>(tab_widget->widget(tabAt(event->pos())));
    if (tab)
        tab->context_menu()->exec(event->globalPos());
}

TabWidget::TabWidget(QWidget* parent)
    : QTabWidget(parent)
{
    // This must be called first, otherwise several of the options below have no effect.
    setTabBar(new TabBar(this));

    setDocumentMode(true);
    setElideMode(Qt::TextElideMode::ElideRight);
    setMovable(true);
    setTabsClosable(true);

    setStyle(new TabStyle(this));

    installEventFilter(parent);
}

TabBarButton::TabBarButton(QIcon const& icon, QWidget* parent)
    : QPushButton(icon, {}, parent)
{
    resize({ 20, 20 });
    setFlat(true);
}

bool TabBarButton::event(QEvent* event)
{
    if (event->type() == QEvent::Enter)
        setFlat(false);
    if (event->type() == QEvent::Leave)
        setFlat(true);

    return QPushButton::event(event);
}

TabStyle::TabStyle(QObject* parent)
{
    setParent(parent);
}

QRect TabStyle::subElementRect(QStyle::SubElement sub_element, QStyleOption const* option, QWidget const* widget) const
{
    // Place our add-tab button (set as the top-right corner widget) directly after the last tab
    if (sub_element == QStyle::SE_TabWidgetRightCorner) {
        auto* tab_widget = verify_cast<TabWidget>(widget);
        auto tab_bar_size = tab_widget->tabBar()->sizeHint();
        auto new_tab_button_size = tab_bar_size.height();
        return QRect {
            qMin(tab_bar_size.width(), tab_widget->width() - new_tab_button_size),
            0,
            new_tab_button_size,
            new_tab_button_size
        };
    }

    return QProxyStyle::subElementRect(sub_element, option, widget);
}

}
