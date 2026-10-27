/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Vector.h>
#include <UI/Qt/Popover.h>

#include <QIcon>

class QLineEdit;
class QScrollArea;

namespace Ladybird {

class AllTabsList;

// Every tab in the window, with a field that searches them.
class AllTabsPopover final : public Popover {
public:
    AK_ALLOC_WITH_KMALLOC;

    explicit AllTabsPopover(QWidget* parent);

    struct Entry {
        QIcon icon;
        QString title;
        QString detail;
        bool is_current { false };
    };
    void set_tabs(Vector<Entry>);
    void set_recently_closed(Vector<Entry>);
    void update_chrome_style(QPalette const&);

    Function<void(size_t)> on_tab_activated;
    Function<void(size_t)> on_recently_closed_activated;

private:
    virtual void showEvent(QShowEvent*) override;
    virtual bool eventFilter(QObject*, QEvent*) override;

    void update_search();

    QLineEdit* m_search_field { nullptr };
    QScrollArea* m_scroll_area { nullptr };
    AllTabsList* m_tab_list { nullptr };
    QWidget* m_recently_closed_section { nullptr };
    AllTabsList* m_recently_closed_list { nullptr };
};

}
