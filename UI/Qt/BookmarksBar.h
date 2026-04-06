/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <LibWebView/Forward.h>

#include <QToolBar>

namespace Ladybird {

class BookmarksBar final : public QToolBar {
    Q_OBJECT

public:
    explicit BookmarksBar(QWidget* parent = nullptr);

    void rebuild();

    String const& selected_bookmark_menu_item_id() const { return m_selected_bookmark_menu_item_id; }
    Optional<String> const& selected_bookmark_menu_target_folder_id() const { return m_selected_bookmark_menu_target_folder_id; }

    void show_context_menu(QPoint, Optional<WebView::BookmarkItem const&>, Optional<String const&> target_folder_id);

private:
    virtual bool eventFilter(QObject* object, QEvent* event) override;

    bool handle_left_mouse_click(QMouseEvent*, QObject*);
    bool handle_middle_mouse_click(QMouseEvent*, QObject*);
    bool handle_right_mouse_click(QMouseEvent*, QObject*);
    void extract_item_properties(QObject*);

    QMenu& bookmarks_bar_context_menu();
    QMenu& bookmark_context_menu();
    QMenu& bookmark_folder_context_menu();

    QMenu* m_bookmarks_bar_context_menu { nullptr };
    QMenu* m_bookmark_context_menu { nullptr };
    QMenu* m_bookmark_folder_context_menu { nullptr };

    String m_selected_bookmark_menu_item_id;
    QString m_selected_bookmark_menu_item_type;
    Optional<String> m_selected_bookmark_menu_target_folder_id;
};

}
