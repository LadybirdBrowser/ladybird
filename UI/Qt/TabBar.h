/*
 * Copyright (c) 2024-2026, Tim Flynn <trflynn89@ladybird.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/TypeCasts.h>

#include <QPointer>
#include <QPushButton>
#include <QStackedWidget>
#include <QTabBar>
#include <QWidget>

class QAction;
class QContextMenuEvent;
class QEvent;
class QIcon;
class QToolButton;

namespace Ladybird {

class Tab;
class TabWidget;

class TabBar final : public QTabBar {
    Q_OBJECT

public:
    explicit TabBar(TabWidget*);

    void set_available_width(int width);

    virtual QSize tabSizeHint(int index) const override;
    virtual void contextMenuEvent(QContextMenuEvent* event) override;

private:
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;

    QPointer<TabWidget> m_tab_widget;

    int m_available_width { 0 };
    int m_x_position_in_selected_tab_while_dragging { 0 };
};

class TabWidget final : public QWidget {
    Q_OBJECT

public:
    explicit TabWidget(QWidget* parent = nullptr);

    TabBar* tab_bar() const { return m_tab_bar; }

    void add_tab(Tab* widget, QString const& label);
    void remove_tab(int index);

    int count() const { return m_tab_bar->count(); }

    int current_index() const { return m_tab_bar->currentIndex(); }
    void set_current_index(int index) { m_tab_bar->setCurrentIndex(index); }

    Tab* tab(int index) const { return as<Tab>(m_stacked_widget->widget(index)); }
    void set_current_tab(Tab*);

    int index_of(Tab* widget) const;

    void set_tab_text(int index, QString const& text) { m_tab_bar->setTabText(index, text); }
    void set_tab_tooltip(int index, QString const& tip) { m_tab_bar->setTabToolTip(index, tip); }
    void set_tab_icon(int index, QIcon const& icon) { m_tab_bar->setTabIcon(index, icon); }

    void set_tab_bar_visible(bool visible) { m_tab_bar_row->setVisible(visible); }
    void set_new_tab_action(QAction* action);

signals:
    void current_tab_changed(int index);
    void tab_close_requested(int index);

protected:
    virtual bool event(QEvent* event) override;
    virtual void resizeEvent(QResizeEvent*) override;

private:
    void update_tab_layout();
    void recreate_icons();

    TabBar* m_tab_bar { nullptr };
    QStackedWidget* m_stacked_widget { nullptr };
    QToolButton* m_new_tab_button { nullptr };
    QWidget* m_tab_bar_row { nullptr };
};

class TabBarButton final : public QPushButton {
    Q_OBJECT

public:
    explicit TabBarButton(QIcon const& icon, QWidget* parent = nullptr);

protected:
    virtual bool event(QEvent* event) override;
};

}
