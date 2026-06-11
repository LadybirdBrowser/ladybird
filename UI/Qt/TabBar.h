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
class QBoxLayout;
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
class QResizeEvent;
class QToolButton;
class QTimer;
class QVariantAnimation;
class QWheelEvent;

namespace Ladybird {

class Tab;
class TabPreviewPopup;
class TabWidget;

enum class TabLayout : u8 {
    Horizontal,
    VerticalCollapsed,
    VerticalExpanded,
};

class TabBar final : public QTabBar {
    Q_OBJECT

public:
    explicit TabBar(TabWidget*);

    void set_available_width(int width);

    TabLayout tab_layout() const { return m_tab_layout; }
    void set_tab_layout(TabLayout);
    void refresh_tab_layout();
    void recreate_icons();

private:
    virtual QSize sizeHint() const override;
    virtual QSize minimumSizeHint() const override;
    virtual QSize tabSizeHint(int index) const override;

    virtual void resizeEvent(QResizeEvent*) override;
    virtual void tabLayoutChange() override;

    virtual bool event(QEvent*) override;
    virtual void paintEvent(QPaintEvent*) override;
    virtual void contextMenuEvent(QContextMenuEvent* event) override;
    virtual void dragEnterEvent(QDragEnterEvent*) override;
    virtual void dragLeaveEvent(QDragLeaveEvent*) override;
    virtual void dragMoveEvent(QDragMoveEvent*) override;
    virtual void dropEvent(QDropEvent*) override;
    virtual bool eventFilter(QObject*, QEvent*) override;
    virtual void leaveEvent(QEvent*) override;
    virtual void mouseDoubleClickEvent(QMouseEvent*) override;
    virtual void mousePressEvent(QMouseEvent*) override;
    virtual void mouseMoveEvent(QMouseEvent*) override;
    virtual void mouseReleaseEvent(QMouseEvent*) override;
    virtual void wheelEvent(QWheelEvent*) override;

    int insertion_index_at(QPoint const&) const;
    int drop_indicator_index_for_insertion_index(int insertion_index) const;
    QPixmap render_tab_drag_pixmap(int index) const;
    void toggle_window_maximized();
    bool start_window_move();
    void start_tab_drag(int index);
    void start_hover_animation(int tab_index, qreal target_progress);
    void schedule_tab_preview(int index);
    void show_tab_preview();
    void hide_tab_preview();
    QPoint tab_preview_position_for(int index, QSize const& popup_size) const;

    QRect visual_tab_rect(int index) const;
    int tab_index_at(QPoint const&) const;
    QSize vertical_size_hint(int tab_count) const;

    int max_vertical_scroll_offset() const;
    void set_vertical_scroll_offset(int);

    void set_hovered_tab_index(int);
    void ensure_tab_visible(int index);
    void update_tab_button_geometry();

    QPointer<TabWidget> m_tab_widget;

    TabLayout m_tab_layout { TabLayout::Horizontal };
    int m_available_width { 0 };
    int m_hovered_tab_index { -1 };
    int m_hover_animation_tab_index { -1 };
    int m_vertical_scroll_offset { 0 };
    qreal m_hover_progress { 0.0 };
    int m_drop_indicator_index { -1 };
    QVariantAnimation* m_hover_animation { nullptr };
    QIcon m_fallback_tab_icon;
    QPointer<Tab> m_pressed_tab;
    QPoint m_position_in_selected_tab_while_dragging;
    QPoint m_drag_start_position;
    QTimer* m_tab_preview_timer { nullptr };
    TabPreviewPopup* m_tab_preview_popup { nullptr };
    int m_tab_preview_index { -1 };
};

class TabWidget final : public QWidget {
    Q_OBJECT

public:
    explicit TabWidget(QWidget* parent = nullptr);

    TabBar* tab_bar() const { return m_tab_bar; }
    QWidget* tab_bar_row() const { return m_tab_bar_row; }
    QRect tab_strip_global_rect() const;

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

    void set_tab_bar_visible(bool visible);
    void set_new_tab_action(QAction* action);
    void set_window_controls_visible(bool);
    void update_window_button_icons();
    void set_vertical_tabs_enabled(bool);
    void set_vertical_tabs_expanded(bool);
    void set_vertical_tabs_expand_on_hover(bool);
    void update_tab_button_visibility();

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
    TabLayout current_tab_layout() const;
    QWidget* tab_drag_area_widget() const;
    bool vertical_tabs_effectively_expanded() const;
    bool can_expand_vertical_tabs_on_hover() const;
    bool cursor_is_over_vertical_tabs() const;
    int vertical_tabs_layout_width() const;
    bool should_show_window_controls_in_tab_toolbar() const;

    void rebuild_layout();
    void rebuild_layout_for_horizontal_tabs();
    void rebuild_layout_for_vertical_tabs();
    void rebuild_page_column();
    void update_toolbar_placement();
    void update_tab_toolbar_window_controls_visibility();
    int current_vertical_tabs_width() const;
    void apply_vertical_tabs_expanded_width(int width);
    void persist_vertical_tabs_expanded_width();
    void update_vertical_tabs_resize_handle();
    void update_vertical_tabs_content_separator();
    void set_resize_handle_property(char const* property, bool enabled);
    void update_vertical_tabs_action_labels();
    void update_vertical_tabs_hover_layout();
    QRect vertical_tabs_chrome_rect() const;
    int vertical_tabs_tab_width() const;
    void update_vertical_tabs_button_layout();
    void update_tab_layout();
    void update_tab_chrome_visibility();
    void recreate_icons();
    void update_chrome_style();
    void update_vertical_tabs_overlay_geometry();
    void set_vertical_tabs_hover_expanded(bool);
    void defer_update_vertical_tabs_hover_expanded();
    void update_vertical_tabs_hover_expanded();
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
    QWidget* m_window_controls { nullptr };
    QStackedWidget* m_toolbar_container { nullptr };
    QWidget* m_page_column { nullptr };
    QWidget* m_tab_bar_row { nullptr };
    QWidget* m_vertical_tabs_reserved_space { nullptr };
    QWidget* m_vertical_tab_bar_column { nullptr };
    QWidget* m_vertical_tabs_content_separator { nullptr };
    QWidget* m_vertical_tabs_resize_handle { nullptr };
    QWidget* m_vertical_tabs_content { nullptr };
    QTimer* m_vertical_tabs_hover_collapse_timer { nullptr };
    QBoxLayout* m_main_layout { nullptr };
    QBoxLayout* m_tab_bar_row_layout { nullptr };
    QBoxLayout* m_page_column_layout { nullptr };
    QBoxLayout* m_vertical_tab_bar_column_layout { nullptr };
    QBoxLayout* m_vertical_tabs_content_layout { nullptr };
    bool m_tab_bar_visible { true };
    bool m_window_controls_visible { true };
    bool m_vertical_tabs_enabled { false };
    bool m_vertical_tabs_expanded { true };
    bool m_vertical_tabs_expand_on_hover { false };
    bool m_vertical_tabs_hover_expanded { false };
    bool m_is_resizing_vertical_tabs { false };
    bool m_is_updating_chrome_style { false };
    int m_vertical_tabs_expanded_width { 0 };
    int m_vertical_tabs_resize_start_global_x { 0 };
    int m_vertical_tabs_resize_start_width { 0 };
};

class TabBarButton final : public QPushButton {
    Q_OBJECT

public:
    explicit TabBarButton(QIcon const& icon, QWidget* parent = nullptr);
    void set_collapsed_vertical_overlay(bool);

protected:
    virtual bool event(QEvent* event) override;
};

}
