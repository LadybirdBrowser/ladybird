/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWebView/HistoryStore.h>
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

static Vector<QAction*> recent_history_actions(QMenu& menu)
{
    Vector<QAction*> actions;
    for (auto* action : menu.actions()) {
        if (!action->isSeparator() && action->text() != QStringLiteral("Back") && action->text() != QStringLiteral("Forward"))
            actions.append(action);
    }
    return actions;
}

static WebView::HistoryEntry history_entry(StringView url, StringView title)
{
    WebView::HistoryEntry entry;
    entry.url = MUST(String::from_utf8(url));
    entry.title = MUST(String::from_utf8(title));
    return entry;
}

TEST_CASE(history_menu_keeps_its_entries_when_they_are_unchanged)
{
    QMenu menu;
    QAction back(QStringLiteral("Back"), &menu);
    QAction forward(QStringLiteral("Forward"), &menu);
    Ladybird::add_history_navigation_actions(menu, back, forward);

    QObject first_view;
    Vector<String> navigations;
    auto navigate = [&navigations](URL::URL const& url) { navigations.append(url.serialize()); };
    Vector<WebView::HistoryEntry> entries { history_entry("https://example.com/"sv, "Example"sv), history_entry("https://ladybird.org/"sv, "Ladybird"sv) };

    Ladybird::update_history_menu(menu, &first_view, entries, navigate);
    auto actions = recent_history_actions(menu);
    EXPECT_EQ(actions.size(), 2u);
    auto icon_key = actions[1]->icon().cacheKey();

    // Preparing the menu again for the same entries leaves its actions as they are.
    Ladybird::update_history_menu(menu, &first_view, entries, navigate);
    EXPECT(recent_history_actions(menu) == actions);
    EXPECT_EQ(actions[1]->icon().cacheKey(), icon_key);

    actions.first()->trigger();
    EXPECT_EQ(navigations.size(), 1u);
    EXPECT_EQ(navigations.first(), "https://example.com/"sv);

    // A new navigator for the same entries keeps the actions, which then navigate through it.
    Vector<String> other_navigations;
    Ladybird::update_history_menu(menu, &first_view, entries, [&other_navigations](URL::URL const& url) { other_navigations.append(url.serialize()); });
    EXPECT(recent_history_actions(menu) == actions);
    actions.first()->trigger();
    EXPECT_EQ(navigations.size(), 1u);
    EXPECT_EQ(other_navigations.size(), 1u);
    Ladybird::update_history_menu(menu, &first_view, entries, navigate);

    // A changed entry is shown in the action that showed it before.
    entries[1].title = "Ladybird browser"_string;
    Ladybird::update_history_menu(menu, &first_view, entries, navigate);
    EXPECT(recent_history_actions(menu) == actions);
    EXPECT(actions[1]->text() == QStringLiteral("Ladybird browser"));

    // So are the entries of another view, which its actions then navigate.
    QObject second_view;
    Vector<String> second_navigations;
    Ladybird::update_history_menu(menu, &second_view, entries, [&second_navigations](URL::URL const& url) { second_navigations.append(url.serialize()); });
    EXPECT(recent_history_actions(menu) == actions);
    actions[1]->trigger();
    EXPECT_EQ(navigations.size(), 1u);
    EXPECT_EQ(second_navigations.size(), 1u);
    EXPECT_EQ(second_navigations.first(), "https://ladybird.org/"sv);

    // A new palette redraws the icons of entries without a favicon.
    auto icon_key_before_palette_change = actions[1]->icon().cacheKey();
    auto palette = menu.palette();
    palette.setColor(QPalette::WindowText, palette.color(QPalette::WindowText) == Qt::red ? Qt::blue : Qt::red);
    menu.setPalette(palette);
    Ladybird::update_history_menu(menu, &second_view, entries, navigate);
    EXPECT_NE(actions[1]->icon().cacheKey(), icon_key_before_palette_change);

    // An entry whose favicon is empty shows no icon, and one without a favicon shows the fallback icon again.
    entries[1].favicon_png = ByteBuffer {};
    Ladybird::update_history_menu(menu, &second_view, entries, navigate);
    EXPECT(actions[1]->icon().isNull());
    entries[1].favicon_png = {};
    Ladybird::update_history_menu(menu, &second_view, entries, navigate);
    EXPECT(!actions[1]->icon().isNull());

    // Fewer entries leave fewer actions, and no view leaves just Back and Forward.
    entries.take_last();
    Ladybird::update_history_menu(menu, &second_view, entries, navigate);
    EXPECT_EQ(recent_history_actions(menu).size(), 1u);
    EXPECT_EQ(recent_history_actions(menu).first(), actions.first());
    Ladybird::update_history_menu(menu, nullptr, {}, {});
    EXPECT(recent_history_actions(menu).is_empty());

    // A view that goes away leaves no entries behind, even when the menu is next prepared with the same ones.
    {
        QObject short_lived_view;
        Ladybird::update_history_menu(menu, &short_lived_view, entries, navigate);
        EXPECT_EQ(recent_history_actions(menu).size(), 1u);
    }
    Ladybird::update_history_menu(menu, nullptr, entries, {});
    EXPECT(recent_history_actions(menu).is_empty());
}
