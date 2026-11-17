/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonObject.h>
#include <LibTest/TestCase.h>
#include <UI/Qt/LocationEdit.h>

#include <QAction>
#include <QCoreApplication>
#include <QToolButton>
#include <QWidget>

namespace {

class EmptySuggestionProvider final : public WebView::OmniboxSuggestionProvider {
public:
    AK_ALLOC_WITH_KMALLOC;

    virtual void query(WebView::AutocompleteQueryID query_id, String, size_t) override
    {
        on_suggestions(query_id, {}, WebView::AutocompleteResultKind::Final);
    }

    virtual void cancel() override { }
};

}

TEST_CASE(zoom_indicator_hides_while_editing_or_when_the_url_is_hidden)
{
    auto settings = WebView::Settings::create_from_json(JsonObject {});
    QWidget window;
    Ladybird::LocationEdit field(&window, settings, make<EmptySuggestionProvider>());
    window.show();
    window.activateWindow();
    QCoreApplication::processEvents();
    field.clearFocus();
    auto* zoom = new QAction("110%", &field);
    field.set_zoom_action(zoom);
    auto* zoom_button = field.findChild<QToolButton*>("LadybirdLocationZoomIndicator");
    VERIFY(zoom_button);
    EXPECT(zoom_button->isVisible());

    field.setFocus();
    EXPECT(field.hasFocus());
    EXPECT(zoom_button->isHidden());
    zoom->setText("120%");
    EXPECT(zoom_button->isHidden());

    field.clearFocus();
    EXPECT(zoom_button->isVisible());
    EXPECT(zoom_button->text() == QStringLiteral("120%"));

    field.set_url_is_hidden(true);
    EXPECT(!field.is_editing_location());
    EXPECT(zoom_button->isHidden());
    zoom->setText("130%");
    EXPECT(zoom_button->isHidden());

    field.set_url_is_hidden(false);
    EXPECT(zoom_button->isVisible());
    EXPECT(zoom_button->text() == QStringLiteral("130%"));
}
