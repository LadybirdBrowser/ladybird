/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/kmalloc.h>
#include <LibWebView/Forward.h>

#include <QPointer>
#include <QToolBar>

class QTimer;
class QToolButton;
class QVariantAnimation;

namespace Ladybird {

struct BookmarkItem;

class Tab;
class WebContentView;
class WindowToolbar;

class BookmarksBar final : public QToolBar {
    Q_OBJECT

public:
    AK_ALLOC_WITH_KMALLOC;

    explicit BookmarksBar(QWidget* parent);
    virtual ~BookmarksBar() override;

    void rebuild();
    void update_chrome_style();

    String const& selected_bookmark_menu_item_id() const { return m_selected_bookmark_menu_item_id; }
    Optional<String> const& selected_bookmark_menu_target_folder_id() const { return m_selected_bookmark_menu_target_folder_id; }
    Optional<String> const& selected_bookmark_menu_parent_folder_id() const { return m_selected_bookmark_menu_parent_folder_id; }

    void show_context_menu(QPoint, Optional<BookmarkItem const&>, Optional<String const&> target_folder_id, Optional<String const&> parent_folder_id);

    bool is_dragging_bookmark() const { return !m_dragged_item_id.isEmpty(); }

    QMenu* overflow_menu() const;

private:
    virtual bool eventFilter(QObject* object, QEvent* event) override;
    virtual void paintEvent(QPaintEvent*) override;

    bool handle_left_mouse_click(QMouseEvent*, QObject*);
    bool handle_middle_mouse_click(QMouseEvent*, QObject*);
    bool handle_right_mouse_click(QMouseEvent*, QObject*);
    void extract_item_properties(QObject*);

    struct DropTarget {
        size_t index { 0 };
        QRect indicator_rect;
        QPointer<QAction> folder_action;

        bool operator==(DropTarget const&) const = default;
    };
    DropTarget drop_target_on_bar(QPoint) const;
    void show_drop_target(DropTarget const&);
    void hide_drop_target();

    int set_up_bookmark_button(QToolButton&, QString const& title);
    QPixmap bookmark_button_image(QIcon const&, QString const& title);

    void cancel_press();
    bool is_past_drag_distance(QMouseEvent const&) const;
    void start_drag();
    bool handle_drag_event(QObject const&, QEvent&);
    void update_drag(QPoint global_position);
    QMenu* drag_menu_at(QPoint global_position) const;
    void drop_dragged_item(QPoint global_position);
    void stop_drag();

    void slide_in(QWidget&, int width);
    void slide_out(int index, QPixmap const& image);

    WebContentView* current_view() const;

    bool m_has_been_built { false };
    QPointer<QMenu> m_context_menu;

    void report_context_menu_item() const;

    String m_selected_bookmark_menu_item_id;
    QString m_selected_bookmark_menu_item_type;
    Optional<String> m_selected_bookmark_menu_target_folder_id;
    Optional<String> m_selected_bookmark_menu_parent_folder_id;

    bool m_opened_new_tab_on_menu_press { false };

    QPointer<QToolButton> m_overflow_button;
    QPointer<QToolButton> m_pressed_button;
    QPointer<QMenu> m_pressed_menu;
    QPointer<QAction> m_pressed_menu_action;
    QPoint m_press_position;
    QString m_dragged_item_id;
    QPointer<QToolButton> m_dragged_button;
    QPointer<QMenu> m_drag_source_menu;
    QWidget* m_drag_card { nullptr };
    int m_drag_card_pointer_offset { 0 };
    DropTarget m_drop_target;
};

// A window shows one bookmarks bar, above the selected tab's page, or over it while the pointer is near the toolbar.
class BookmarksBarContainer final : public QWidget {
    Q_OBJECT

public:
    AK_ALLOC_WITH_KMALLOC;

    BookmarksBarContainer(WindowToolbar&, QWidget& page_stack, QWidget* parent);

    BookmarksBar& bar() { return *m_bar; }

    void bind(Tab*);
    void set_sidebar(QWidget&);
    bool shows_over_page() const;
    void update_geometry();

private:
    virtual bool event(QEvent*) override;
    virtual bool eventFilter(QObject*, QEvent*) override;

    bool can_expand_on_hover() const;
    bool cursor_is_over_hover_region() const;
    void update_hover();
    void set_hover_expanded(bool);
    void set_view_overlay_height(int);

    WindowToolbar& m_toolbar;
    QWidget& m_page_stack;
    QWidget* m_sidebar { nullptr };
    BookmarksBar* m_bar { nullptr };
    QPointer<Tab> m_tab;

    QTimer* m_hover_expand_timer { nullptr };
    QTimer* m_hover_collapse_timer { nullptr };
    QVariantAnimation* m_hover_expand_animation { nullptr };
    bool m_hover_expanded { false };
    qreal m_hover_expand_progress { 0.0 };
};

}
