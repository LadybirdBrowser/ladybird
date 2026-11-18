/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <UI/Qt/Menu.h>

#include <QAction>
#include <QMenu>
#include <QPushButton>

TEST_CASE(zoom_percentage_updates_while_menu_is_open)
{
    QMenu menu;
    QAction zoom_out(&menu);
    QAction reset_zoom(&menu);
    QAction zoom_in(&menu);
    auto confirmed_zoom = Ladybird::Action::create("100%"sv, Ladybird::ActionID::ResetZoomViaToolbar, [] { });
    confirmed_zoom->set_visible(false);
    Ladybird::ActionBinding zoom_level(menu, Ladybird::IncludeActionIcon::No);
    zoom_level.bind(*confirmed_zoom);
    auto* row = new Ladybird::MenuZoomRow(menu, zoom_out, reset_zoom, zoom_in, zoom_level.action());
    menu.addAction(row);
    auto* percentage = row->defaultWidget()->findChild<QPushButton*>("LadybirdMenuZoomLevel");
    VERIFY(percentage);

    QPushButton* zoom_in_button = nullptr;
    for (auto* button : row->defaultWidget()->findChildren<QPushButton*>("LadybirdMenuZoomButton")) {
        if (button->accessibleName() == QStringLiteral("Zoom In"))
            zoom_in_button = button;
    }
    int zoom_in_triggers = 0;
    QObject::connect(&zoom_in, &QAction::triggered, &menu, [&zoom_in_triggers] { ++zoom_in_triggers; });

    menu.popup({ 0, 0 });
    EXPECT(percentage->text() == QStringLiteral("100%"));

    VERIFY(zoom_in_button && zoom_in_button->isVisible());
    zoom_in_button->click();
    EXPECT_EQ(zoom_in_triggers, 1);
    EXPECT(percentage->text() == QStringLiteral("100%"));
    confirmed_zoom->set_text("110%"sv);
    confirmed_zoom->set_visible(true);
    EXPECT(percentage->text() == QStringLiteral("110%"));

    confirmed_zoom->set_text("120%"sv);
    EXPECT(percentage->text() == QStringLiteral("120%"));

    confirmed_zoom->set_visible(false);
    EXPECT(percentage->text() == QStringLiteral("100%"));
}
