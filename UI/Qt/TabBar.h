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
class QDragEnterEvent;
class QDragLeaveEvent;
class QDragMoveEvent;
class QDropEvent;
class QEvent;
class QIcon;
class QMouseEvent;
class QPaintEvent;
class QPixmap;
class QToolButton;
class QVariantAnimation;

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
    virtual void dragEnterEvent(QDragEnterEvent*) override;
    virtual void dragLeaveEvent(QDragLeaveEvent*) override;
    virtual void dragMoveEvent(QDragMoveEvent*) override;
    virtual void dropEvent(QDropEvent*) override;
    virtual void paintEvent(QPaintEvent*) override;
    virtual void leaveEvent(QEvent*) override;
    virtual void mouseDoubleClickEvent(QMouseEvent*) override;
    virtual void mousePressEvent(QMouseEvent*) override;
    virtual void mouseMoveEvent(QMouseEvent*) override;
    virtual void mouseReleaseEvent(QMouseEvent*) override;

    int insertion_index_at(QPoint const&) const;
    int drop_indicator_index_for_insertion_index(int insertion_index) const;
    QPixmap render_tab_drag_pixmap(int index) const;
    void toggle_window_maximized();
    void start_tab_drag(int index);
    void start_hover_animation(int tab_index, qreal target_progress);

    QPointer<TabWidget> m_tab_widget;

    int m_available_width { 0 };
    int m_hovered_tab_index { -1 };
    int m_hover_animation_tab_index { -1 };
    qreal m_hover_progress { 0.0 };
    int m_drop_indicator_index { -1 };
    QVariantAnimation* m_hover_animation { nullptr };
    QPointer<Tab> m_pressed_tab;
    QPoint m_position_in_selected_tab_while_dragging;
    QPoint m_drag_start_position;
};

class TabWidget final : public QWidget {
    Q_OBJECT

public:
    explicit TabWidget(QWidget* parent = nullptr);

    TabBar* tab_bar() const { return m_tab_bar; }
    QWidget* tab_bar_row() const { return m_tab_bar_row; }

    void add_tab(Tab* widget, QString const& label);
    void insert_tab(int index, Tab* widget, QString const& label);
    void remove_tab(int index);
    Tab* take_tab(int index);

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
    void set_window_controls_visible(bool);

signals:
    void current_tab_changed(int index);
    void tab_close_requested(int index);

protected:
    virtual void dragEnterEvent(QDragEnterEvent*) override;
    virtual void dragMoveEvent(QDragMoveEvent*) override;
    virtual void dropEvent(QDropEvent*) override;
    virtual bool event(QEvent* event) override;
    virtual bool eventFilter(QObject*, QEvent*) override;
    virtual void resizeEvent(QResizeEvent*) override;

private:
    void update_tab_layout();
    void recreate_icons();
    void update_chrome_style();
    void update_window_button_icons();
    void toggle_window_maximized();
    bool start_window_move();
    void accept_tab_drag(QDragMoveEvent*);
    void accept_tab_drop(QDropEvent*, int index);

    TabBar* m_tab_bar { nullptr };
    QStackedWidget* m_stacked_widget { nullptr };
    QToolButton* m_new_tab_button { nullptr };
    QToolButton* m_minimize_window_button { nullptr };
    QToolButton* m_maximize_window_button { nullptr };
    QToolButton* m_close_window_button { nullptr };
    QWidget* m_tab_bar_row { nullptr };
    bool m_is_updating_chrome_style { false };
};

class TabBarButton final : public QPushButton {
    Q_OBJECT

public:
    explicit TabBarButton(QIcon const& icon, QWidget* parent = nullptr);

protected:
    virtual bool event(QEvent* event) override;
};

}
