/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>

#include <QToolBar>

namespace Ladybird {

class BookmarksBar final : public QToolBar {
    Q_OBJECT

public:
    explicit BookmarksBar(QWidget* parent = nullptr);

    void rebuild();

    String const& bookmark_context_menu_item_id() const { return m_bookmark_context_menu_item_id; }
    Optional<String> const& bookmark_context_menu_target_folder_id() const { return m_bookmark_context_menu_target_folder_id; }

private:
    bool eventFilter(QObject* object, QEvent* event) override;

    QMenu& bookmarks_bar_context_menu();
    QMenu& bookmark_context_menu();
    QMenu& bookmark_folder_context_menu();

    QMenu* m_bookmarks_bar_context_menu { nullptr };
    QMenu* m_bookmark_context_menu { nullptr };
    QMenu* m_bookmark_folder_context_menu { nullptr };

    String m_bookmark_context_menu_item_id;
    Optional<String> m_bookmark_context_menu_target_folder_id;
};

}
