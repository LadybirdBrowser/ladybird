/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <UI/Qt/HiddenPageSizing.h>

#include <QApplication>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace {

// The shape of a tab in the browser window: A page in a stacked widget, with a layout that gives one child (the
// web view) the whole page.
struct Page {
    explicit Page(QStackedWidget& stack)
        : widget(*new QWidget)
        , child(*new QWidget(&widget))
    {
        auto* layout = new QVBoxLayout(&widget);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(&child);
        stack.addWidget(&widget);
    }

    QWidget& widget;
    QWidget& child;
};

}

// A page that isn't the stacked widget's current page gets no geometry from it, and Qt doesn't lay a hidden page out
// until it is shown — so its child keeps the default 100x30 after the window is shown and sized. That's the condition
// the helper exists for — so the test pins it down before exercising the helper.
TEST_CASE(hidden_page_follows_the_current_page_without_being_shown)
{
    QWidget window;
    auto* stack = new QStackedWidget(&window);
    auto* window_layout = new QVBoxLayout(&window);
    window_layout->setContentsMargins(0, 0, 0, 0);
    window_layout->addWidget(stack);

    Page current(*stack);
    Page hidden(*stack);
    stack->setCurrentWidget(&current.widget);

    window.resize(800, 600);
    window.show();
    QApplication::processEvents();

    EXPECT_EQ(current.child.size(), QSize(800, 600));
    EXPECT(!hidden.widget.isVisible());
    EXPECT_NE(hidden.child.size(), current.child.size());

    Ladybird::size_hidden_page_like(hidden.widget, current.widget);

    EXPECT(!hidden.widget.isVisible());
    EXPECT_EQ(hidden.widget.size(), current.widget.size());
    EXPECT_EQ(hidden.child.size(), current.child.size());

    // And the size sticks when the page is finally shown: Qt's deferred layout pass agrees with what was set.
    stack->setCurrentWidget(&hidden.widget);
    QApplication::processEvents();
    EXPECT_EQ(hidden.child.size(), QSize(800, 600));
}
