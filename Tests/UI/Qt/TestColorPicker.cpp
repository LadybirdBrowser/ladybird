/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <UI/Qt/ColorPicker.h>

#include <QColorDialog>
#include <QWidget>

namespace {

struct Report {
    int updates { 0 };
    int closes { 0 };
    Optional<Color> selected_color;
};

void record_reports(Ladybird::ColorPicker& picker, Report& report)
{
    picker.on_update = [&report](Optional<Color> color, Web::HTML::ColorPickerUpdateState state) {
        if (state == Web::HTML::ColorPickerUpdateState::Update) {
            ++report.updates;
            return;
        }
        ++report.closes;
        report.selected_color = color;
    };
}

}

TEST_CASE(replacing_a_picker_ignores_its_stale_completion)
{
    QWidget web_view;
    web_view.resize(800, 600);
    web_view.show();

    Ladybird::ColorPicker picker(web_view);
    Report report;
    record_reports(picker, report);

    picker.open(Color::Red);
    auto dialogs = web_view.findChildren<QColorDialog*>();
    EXPECT_EQ(dialogs.size(), 1);
    QPointer<QColorDialog> first_dialog = dialogs.first();

    picker.open(Color::Blue);
    dialogs = web_view.findChildren<QColorDialog*>();
    EXPECT_EQ(dialogs.size(), 2);
    auto* second_dialog = dialogs.first() == first_dialog ? dialogs.last() : dialogs.first();

    EXPECT(!first_dialog->isVisible());
    EXPECT(second_dialog->isVisible());
    auto updates_before_stale_change = report.updates;
    first_dialog->setCurrentColor(Qt::green);
    EXPECT_EQ(report.updates, updates_before_stale_change);
    second_dialog->reject();
    EXPECT_EQ(report.closes, 1);

    // Completing the stale picker must not emit another result or consult the now-closed replacement's state.
    first_dialog->accept();
    EXPECT_EQ(report.closes, 1);
    EXPECT(!picker.is_open());
}

TEST_CASE(accepting_the_active_picker_reports_its_color)
{
    QWidget web_view;
    Ladybird::ColorPicker picker(web_view);
    Report report;
    record_reports(picker, report);

    picker.open(Color::Red);
    auto* dialog = web_view.findChild<QColorDialog*>();
    EXPECT(dialog);
    dialog->accept();

    EXPECT_EQ(report.closes, 1);
    EXPECT(report.selected_color.has_value());
    EXPECT_EQ(report.selected_color.value(), Color::Red);
    EXPECT(!picker.is_open());
}
