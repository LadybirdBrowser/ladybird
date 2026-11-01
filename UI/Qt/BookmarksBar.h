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
class QVariantAnimation;

namespace Ladybird {

class Tab;
class WebContentView;
class WindowToolbar;

class BookmarksBar final : public QToolBar {
    Q_OBJECT

public:
    AK_ALLOC_WITH_KMALLOC;

    explicit BookmarksBar(QWidget* parent);

    void rebuild();
    void update_chrome_style();

    String const& selected_bookmark_menu_item_id() const { return m_selected_bookmark_menu_item_id; }
    Optional<String> const& selected_bookmark_menu_target_folder_id() const { return m_selected_bookmark_menu_target_folder_id; }
    Optional<String> const& selected_bookmark_menu_parent_folder_id() const { return m_selected_bookmark_menu_parent_folder_id; }

    void show_context_menu(QPoint, Optional<WebView::BookmarkItem const&>, Optional<String const&> target_folder_id, Optional<String const&> parent_folder_id);

private:
    virtual bool eventFilter(QObject* object, QEvent* event) override;

    bool handle_left_mouse_click(QMouseEvent*, QObject*);
    bool handle_middle_mouse_click(QMouseEvent*, QObject*);
    bool handle_right_mouse_click(QMouseEvent*, QObject*);
    void extract_item_properties(QObject*);

    void slide_in(QWidget&, int width);
    void slide_out(int index, QPixmap const& image);

    WebContentView* current_view() const;

    bool m_has_been_built { false };
    QPointer<QMenu> m_context_menu;

    String m_selected_bookmark_menu_item_id;
    QString m_selected_bookmark_menu_item_type;
    Optional<String> m_selected_bookmark_menu_target_folder_id;
    Optional<String> m_selected_bookmark_menu_parent_folder_id;
};

// A window shows one bookmarks bar, above the selected tab's page, or over it while the pointer is near the toolbar.
class BookmarksBarContainer final : public QWidget {
    Q_OBJECT

public:
    AK_ALLOC_WITH_KMALLOC;

    BookmarksBarContainer(WindowToolbar&, QWidget& page_stack, QWidget* parent);

    BookmarksBar& bar() { return *m_bar; }

    void bind(Tab*);
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
    BookmarksBar* m_bar { nullptr };
    QPointer<Tab> m_tab;

    QTimer* m_hover_expand_timer { nullptr };
    QTimer* m_hover_collapse_timer { nullptr };
    QVariantAnimation* m_hover_expand_animation { nullptr };
    bool m_hover_expanded { false };
    qreal m_hover_expand_progress { 0.0 };
};

}
