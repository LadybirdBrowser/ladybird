/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWebView/ProcessType.h>
#include <UI/Qt/ProcessManagerWindow.h>

#include <QLineEdit>
#include <QPalette>
#include <QTabBar>
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

TEST_CASE(search_and_type_filters_keep_matching_child_processes_visible)
{
    Ladybird::ProcessManagerWindow window;
    auto* processes = window.findChild<QTreeWidget*>();
    auto* search = window.findChild<QLineEdit*>();
    auto* filters = window.findChild<QTabBar*>();

    auto* browser = new QTreeWidgetItem(processes, { "Browser", "123" });
    browser->setData(0, Qt::UserRole, static_cast<int>(WebView::ProcessType::Browser));
    auto* page = new QTreeWidgetItem(processes, { "WebContent - Example", "456" });
    page->setData(0, Qt::UserRole, static_cast<int>(WebView::ProcessType::WebContent));
    auto* frame = new QTreeWidgetItem(page, { "WebContent - Child frame", "789" });
    frame->setData(0, Qt::UserRole, static_cast<int>(WebView::ProcessType::WebContent));

    search->setText("CHILD");
    EXPECT(browser->isHidden());
    EXPECT(!page->isHidden());
    EXPECT(!frame->isHidden());
    EXPECT(page->isExpanded());

    search->setText("123");
    EXPECT(!browser->isHidden());
    EXPECT(page->isHidden());

    search->clear();
    filters->setCurrentIndex(1);
    EXPECT(browser->isHidden());
    EXPECT(!page->isHidden());
    EXPECT(!frame->isHidden());

    filters->setCurrentIndex(2);
    EXPECT(!browser->isHidden());
    EXPECT(page->isHidden());

    filters->setCurrentIndex(0);
    EXPECT(!browser->isHidden());
    EXPECT(!page->isHidden());
    EXPECT(!frame->isHidden());
}
