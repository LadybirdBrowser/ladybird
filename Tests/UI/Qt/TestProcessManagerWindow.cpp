/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <UI/Qt/ProcessManagerWindow.h>

#include <QLineEdit>
#include <QPalette>
#include <QTreeWidget>

TEST_CASE(palette_changes_do_not_reenter_style_updates)
{
    Ladybird::ProcessManagerWindow window;
    window.ensurePolished();

    for (auto color : { Qt::black, Qt::white, Qt::black }) {
        auto palette = window.palette();
        palette.setColor(QPalette::Window, color);
        window.setPalette(palette);
        EXPECT(window.styleSheet().isEmpty());
        EXPECT_EQ(window.palette().color(QPalette::Window), QColor(color));
    }

    auto* processes = window.findChild<QTreeWidget*>();
    EXPECT(processes);
    EXPECT_EQ(processes->columnCount(), 4);
}

TEST_CASE(search_keeps_matching_child_processes_visible)
{
    Ladybird::ProcessManagerWindow window;
    auto* processes = window.findChild<QTreeWidget*>();
    auto* search = window.findChild<QLineEdit*>();

    auto* browser = new QTreeWidgetItem(processes, { "Browser", "123" });
    auto* page = new QTreeWidgetItem(processes, { "WebContent - Example", "456" });
    auto* frame = new QTreeWidgetItem(page, { "WebContent - Child frame", "789" });

    search->setText("CHILD");
    EXPECT(browser->isHidden());
    EXPECT(!page->isHidden());
    EXPECT(!frame->isHidden());
    EXPECT(page->isExpanded());

    search->setText("123");
    EXPECT(!browser->isHidden());
    EXPECT(page->isHidden());

    search->setText("  example  ");
    EXPECT(browser->isHidden());
    EXPECT(!page->isHidden());
    EXPECT(frame->isHidden());

    search->setText("no matching process");
    EXPECT(browser->isHidden());
    EXPECT(page->isHidden());
    EXPECT(frame->isHidden());

    search->clear();
    EXPECT(!browser->isHidden());
    EXPECT(!page->isHidden());
    EXPECT(!frame->isHidden());
}
