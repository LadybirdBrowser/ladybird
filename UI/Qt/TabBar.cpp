/*
 * Copyright (c) 2024-2026, Tim Flynn <trflynn89@ladybird.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StdLibExtras.h>
#include <UI/Qt/Icon.h>
#include <UI/Qt/Tab.h>
#include <UI/Qt/TabBar.h>

#include <QContextMenuEvent>
#include <QEvent>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QToolButton>
#include <QVBoxLayout>

namespace Ladybird {

TabBar::TabBar(QWidget* parent)
    : QTabBar(parent)
{
}

void TabBar::set_available_width(int width)
{
    if (m_available_width != width) {
        m_available_width = width;
        updateGeometry();
    }
}

QSize TabBar::tabSizeHint(int index) const
{
    auto hint = QTabBar::tabSizeHint(index);

    if (auto count = this->count(); count > 0) {
        auto width = (m_available_width > 0 ? m_available_width : this->width()) / count;
        width = min(225, width);
        width = max(128, width);

        hint.setWidth(width);
    }

    return hint;
}

void TabBar::contextMenuEvent(QContextMenuEvent* event)
{
    auto* tab_widget = as<TabWidget>(parent());

    if (auto* tab = tab_widget->tab(tabAt(event->pos())))
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
    : QWidget(parent)
{
    m_tab_bar = new TabBar(this);
    m_tab_bar->setDocumentMode(true);
    m_tab_bar->setElideMode(Qt::TextElideMode::ElideRight);
    m_tab_bar->setMovable(true);
    m_tab_bar->setTabsClosable(true);
    m_tab_bar->setExpanding(false);
    m_tab_bar->setUsesScrollButtons(true);
    m_tab_bar->setDrawBase(false);

    m_stacked_widget = new QStackedWidget(this);

    m_new_tab_button = new QToolButton(this);
    m_new_tab_button->setIconSize(QSize(16, 16));
    m_new_tab_button->setAutoRaise(true);
    m_new_tab_button->setToolTip("New Tab");

    recreate_icons();

    auto* tab_bar_row_layout = new QHBoxLayout;
    tab_bar_row_layout->setSpacing(0);
    tab_bar_row_layout->setContentsMargins(0, 0, 0, 0);
    tab_bar_row_layout->addWidget(m_tab_bar);
    tab_bar_row_layout->addWidget(m_new_tab_button);
    tab_bar_row_layout->addStretch(1);

    m_tab_bar_row = new QWidget(this);
    m_tab_bar_row->setLayout(tab_bar_row_layout);

    auto* main_layout = new QVBoxLayout(this);
    main_layout->setSpacing(0);
    main_layout->setContentsMargins(0, 0, 0, 0);
    main_layout->addWidget(m_tab_bar_row);
    main_layout->addWidget(m_stacked_widget, 1);

    connect(m_tab_bar, &QTabBar::currentChanged, this, [this](int index) {
        if (index >= 0 && index < m_stacked_widget->count())
            m_stacked_widget->setCurrentIndex(index);

        emit current_tab_changed(index);
    });

    connect(m_tab_bar, &QTabBar::tabCloseRequested, this, &TabWidget::tab_close_requested);

    connect(m_tab_bar, &QTabBar::tabMoved, this, [this](int from, int to) {
        ScopeGuard guard { [&]() { m_stacked_widget->blockSignals(false); } };
        m_stacked_widget->blockSignals(true);

        auto* widget = m_stacked_widget->widget(from);
        m_stacked_widget->removeWidget(widget);
        m_stacked_widget->insertWidget(to, widget);
        m_stacked_widget->setCurrentIndex(m_tab_bar->currentIndex());
    });
}

void TabWidget::add_tab(Tab* widget, QString const& label)
{
    m_stacked_widget->addWidget(widget);
    m_tab_bar->addTab(label);

    update_tab_layout();
}

void TabWidget::remove_tab(int index)
{
    auto* widget = m_stacked_widget->widget(index);
    m_stacked_widget->removeWidget(widget);

    m_tab_bar->removeTab(index);

    if (m_tab_bar->count() > 0 && m_tab_bar->currentIndex() >= 0)
        m_stacked_widget->setCurrentIndex(m_tab_bar->currentIndex());

    update_tab_layout();
}

void TabWidget::set_current_tab(Tab* widget)
{
    if (auto index = m_stacked_widget->indexOf(widget); index >= 0)
        set_current_index(index);
}

int TabWidget::index_of(Tab* widget) const
{
    return m_stacked_widget->indexOf(widget);
}

void TabWidget::set_new_tab_action(QAction* action)
{
    disconnect(m_new_tab_button, &QToolButton::clicked, nullptr, nullptr);

    if (!action)
        return;

    connect(m_new_tab_button, &QToolButton::clicked, action, &QAction::trigger);
}

bool TabWidget::event(QEvent* event)
{
    if (auto type = event->type(); type == QEvent::MouseButtonRelease) {
        auto const* mouse_event = static_cast<QMouseEvent const*>(event);

        if (mouse_event->button() == Qt::MiddleButton) {
            if (auto index = m_tab_bar->tabAt(mouse_event->pos()); index != -1) {
                emit tab_close_requested(index);
                return true;
            }
        }
    } else if (type == QEvent::PaletteChange) {
        recreate_icons();
    }

    return QWidget::event(event);
}

void TabWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    update_tab_layout();
}

void TabWidget::update_tab_layout()
{
    auto button_width = m_new_tab_button->sizeHint().width();
    auto available_for_tabs = width() - button_width;

    m_tab_bar->set_available_width(available_for_tabs);
}

void TabWidget::recreate_icons()
{
    m_new_tab_button->setIcon(create_tvg_icon_with_theme_colors("new_tab", palette()));
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

}
