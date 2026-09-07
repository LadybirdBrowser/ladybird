/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <UI/Qt/SelectDropdown.h>

#include <QApplication>
#include <QKeyEvent>
#include <QTimer>
#include <QWidget>

namespace {

struct Report {
    int calls { 0 };
    Optional<u32> selected_item_id;
};

void record_reports(Ladybird::SelectDropdown& dropdown, Report& report)
{
    dropdown.on_closed = [&report](Optional<u32> const& selected_item_id) {
        ++report.calls;
        report.selected_item_id = selected_item_id;
    };
}

Vector<Web::HTML::SelectItem> three_options()
{
    Vector<Web::HTML::SelectItem> items;
    items.append(Web::HTML::SelectItemOption { .id = 1, .selected = true, .label = "Alpha"_utf16 });
    items.append(Web::HTML::SelectItemOption { .id = 2, .label = "Bravo"_utf16 });
    items.append(Web::HTML::SelectItemOption { .id = 3, .label = "Charlie"_utf16 });
    return items;
}

void send_key(QWidget& widget, int key)
{
    QKeyEvent press { QEvent::KeyPress, key, Qt::NoModifier };
    QApplication::sendEvent(&widget, &press);
    QKeyEvent release { QEvent::KeyRelease, key, Qt::NoModifier };
    QApplication::sendEvent(&widget, &release);
    QApplication::processEvents();
}

// open() runs the menu's own event loop until the menu closes, so the interaction is queued before the call and runs
// from inside that loop.
template<typename Interaction>
void open_and_interact(Ladybird::SelectDropdown& dropdown, Vector<Web::HTML::SelectItem> const& items, Interaction interaction)
{
    bool interacted = false;
    QTimer::singleShot(0, &dropdown, [&] {
        interacted = true;
        EXPECT(dropdown.isVisible());
        interaction();
    });
    dropdown.open(QPoint { 10, 10 }, 100, items);
    EXPECT(interacted);
    EXPECT(!dropdown.isVisible());
}

}

TEST_CASE(escape_with_an_item_highlighted_reports_no_choice)
{
    QWidget web_view;
    web_view.resize(800, 600);
    web_view.show();

    Ladybird::SelectDropdown dropdown(&web_view);
    Report report;
    record_reports(dropdown, report);

    open_and_interact(dropdown, three_options(), [&] {
        send_key(dropdown, Qt::Key_Down);
        EXPECT(dropdown.activeAction());
        send_key(dropdown, Qt::Key_Escape);
    });

    EXPECT_EQ(report.calls, 1);
    EXPECT(!report.selected_item_id.has_value());
}

TEST_CASE(escape_with_nothing_highlighted_reports_no_choice)
{
    QWidget web_view;
    web_view.resize(800, 600);
    web_view.show();

    Ladybird::SelectDropdown dropdown(&web_view);
    Report report;
    record_reports(dropdown, report);

    open_and_interact(dropdown, three_options(), [&] {
        EXPECT(!dropdown.activeAction());
        send_key(dropdown, Qt::Key_Escape);
    });

    EXPECT_EQ(report.calls, 1);
    EXPECT(!report.selected_item_id.has_value());
}

TEST_CASE(hiding_the_menu_with_an_item_highlighted_reports_no_choice)
{
    QWidget web_view;
    web_view.resize(800, 600);
    web_view.show();

    Ladybird::SelectDropdown dropdown(&web_view);
    Report report;
    record_reports(dropdown, report);

    // The window deactivating is one of the ways a menu goes away from the outside; hide() stands in for all of them.
    open_and_interact(dropdown, three_options(), [&] {
        dropdown.setActiveAction(dropdown.actions()[1]);
        dropdown.hide();
    });

    EXPECT_EQ(report.calls, 1);
    EXPECT(!report.selected_item_id.has_value());
}

TEST_CASE(choosing_an_item_reports_its_id)
{
    QWidget web_view;
    web_view.resize(800, 600);
    web_view.show();

    Ladybird::SelectDropdown dropdown(&web_view);
    Report report;
    record_reports(dropdown, report);

    open_and_interact(dropdown, three_options(), [&] {
        dropdown.setActiveAction(dropdown.actions()[2]);
        send_key(dropdown, Qt::Key_Return);
    });

    EXPECT_EQ(report.calls, 1);
    EXPECT(report.selected_item_id.has_value());
    EXPECT_EQ(report.selected_item_id.value(), 3u);
}

TEST_CASE(choosing_an_item_inside_a_group_reports_its_id)
{
    QWidget web_view;
    web_view.resize(800, 600);
    web_view.show();

    Ladybird::SelectDropdown dropdown(&web_view);
    Report report;
    record_reports(dropdown, report);

    Vector<Web::HTML::SelectItem> items;
    items.append(Web::HTML::SelectItemOption { .id = 1, .selected = true, .label = "Top"_utf16 });
    items.append(Web::HTML::SelectItemSeparator {});
    Vector<Web::HTML::SelectItemOption> grouped_options;
    grouped_options.append(Web::HTML::SelectItemOption { .id = 2, .label = "First"_utf16 });
    grouped_options.append(Web::HTML::SelectItemOption { .id = 3, .label = "Second"_utf16 });
    items.append(Web::HTML::SelectItemOptionGroup { .label = "Group"_utf16, .items = move(grouped_options) });

    open_and_interact(dropdown, items, [&] {
        // The menu holds: Top, a separator, the group's disabled title, First, Second.
        auto actions = dropdown.actions();
        EXPECT(actions.size() == 5);
        dropdown.setActiveAction(actions[4]);
        send_key(dropdown, Qt::Key_Return);
    });

    EXPECT_EQ(report.calls, 1);
    EXPECT(report.selected_item_id.has_value());
    EXPECT_EQ(report.selected_item_id.value(), 3u);
}

TEST_CASE(closing_without_reporting_reports_nothing)
{
    QWidget web_view;
    web_view.resize(800, 600);
    web_view.show();

    Ladybird::SelectDropdown dropdown(&web_view);
    Report report;
    record_reports(dropdown, report);

    open_and_interact(dropdown, three_options(), [&] {
        dropdown.setActiveAction(dropdown.actions()[0]);
        dropdown.close_without_reporting();
    });
    EXPECT_EQ(report.calls, 0);

    // The next menu reports again as normal.
    open_and_interact(dropdown, three_options(), [&] {
        send_key(dropdown, Qt::Key_Escape);
    });
    EXPECT_EQ(report.calls, 1);
    EXPECT(!report.selected_item_id.has_value());
}
