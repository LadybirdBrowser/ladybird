/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StdLibExtras.h>
#include <AK/TypeCasts.h>
#include <UI/Qt/Tab.h>
#include <UI/Qt/TabBar.h>

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
    auto hint = QTabBar::tabSizeHint(index);

    if (auto count = this->count(); count > 0) {
        auto width = this->width() / count;
        width = min(225, width);
        width = max(128, width);

        hint.setWidth(width);
    }

    return hint;
}

void TabBar::contextMenuEvent(QContextMenuEvent* event)
{
    auto* tab_widget = as<QTabWidget>(this->parent());
    auto* tab = as<Tab>(tab_widget->widget(tabAt(event->pos())));
    if (tab)
        tab->context_menu()->exec(event->globalPos());
}

void TabBar::mousePressEvent(QMouseEvent* event)
{
    event->ignore();

    auto rect_of_current_tab = tabRect(tabAt(event->pos()));
    m_x_position_in_selected_tab_while_dragging = event->pos().x() - rect_of_current_tab.x();

    QTabBar::mousePressEvent(event);
}

void TabBar::mouseMoveEvent(QMouseEvent* event)
{
    event->ignore();

    auto rect_of_first_tab = tabRect(0);
    auto rect_of_last_tab = tabRect(count() - 1);

    auto boundary_limit_for_dragging_tab = QRect(rect_of_first_tab.x() + m_x_position_in_selected_tab_while_dragging, 0,
        rect_of_last_tab.x() + m_x_position_in_selected_tab_while_dragging, 0);

    if (event->pos().x() >= boundary_limit_for_dragging_tab.x() && event->pos().x() <= boundary_limit_for_dragging_tab.width()) {
        QTabBar::mouseMoveEvent(event);
    } else {
        auto pos = event->pos();
        if (event->pos().x() > boundary_limit_for_dragging_tab.width())
            pos.setX(boundary_limit_for_dragging_tab.width());
        else if (event->pos().x() < boundary_limit_for_dragging_tab.x())
            pos.setX(boundary_limit_for_dragging_tab.x());
        QMouseEvent ev(event->type(), pos, event->globalPosition(), event->button(), event->buttons(), event->modifiers());
        QTabBar::mouseMoveEvent(&ev);
    }
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

void TabWidget::paintEvent(QPaintEvent*)
{
    auto prepare_style_options = [](QTabBar* tab_bar, QSize widget_size) {
        QStyleOptionTabBarBase style_options;
        QStyleOptionTab tab_overlap;
        tab_overlap.shape = tab_bar->shape();
        auto overlap = tab_bar->style()->pixelMetric(QStyle::PM_TabBarBaseOverlap, &tab_overlap, tab_bar);
        style_options.initFrom(tab_bar);
        style_options.shape = tab_bar->shape();
        style_options.documentMode = tab_bar->documentMode();
        // NOTE: This assumes the tab bar is at the top of the tab widget.
        style_options.rect = { 0, widget_size.height() - overlap, widget_size.width(), overlap };

        return style_options;
    };

    QStylePainter painter { this, tabBar() };
    if (auto* widget = cornerWidget(Qt::TopRightCorner)) {
        // Manually paint the background for the area where the "new tab" button would have been
        // if we hadn't relocated it in `TabStyle::subElementRect()`.
        auto style_options = prepare_style_options(tabBar(), widget->size());
        style_options.rect.translate(tabBar()->rect().width(), widget->y());
        painter.drawPrimitive(QStyle::PE_FrameTabBarBase, style_options);
    }
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
        auto* tab_widget = as<TabWidget>(widget);
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
