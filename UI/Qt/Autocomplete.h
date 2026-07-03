/*
 * Copyright (c) 2023, Cameron Youell <cameronyouell@gmail.com>
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibWebView/Autocomplete.h>

#include <QObject>

class QFrame;
class QLineEdit;
class QListView;

namespace Ladybird {

class AutocompleteModel;
class AutocompleteDelegate;

// The suggestion popup below the location bar. This is a pure view: LocationEdit feeds it rows and a
// selection from the WebView::Omnibox model, and it reports row hovers, clicks, and outside-click
// dismissals back.
class Autocomplete final : public QObject {
    Q_OBJECT

public:
    explicit Autocomplete(QLineEdit* anchor);
    virtual ~Autocomplete() override;

    void update_chrome_style();
    void schedule_chrome_style_update();

    void show_with_suggestions(Vector<WebView::AutocompleteSuggestion>, Optional<size_t> selected_suggestion_index);
    void set_selected_suggestion(Optional<size_t> suggestion_index);
    bool close();

signals:
    void suggestion_clicked(int suggestion_index);
    void suggestion_hovered(int suggestion_index);
    void dismissed();

protected:
    virtual bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void create_popup();
    bool is_visible() const;
    void position_popup();
    bool is_selectable_row(int row) const;

    QLineEdit* m_anchor { nullptr };
    QFrame* m_popup { nullptr };
    QListView* m_list_view { nullptr };
    AutocompleteModel* m_model { nullptr };
    AutocompleteDelegate* m_delegate { nullptr };
    bool m_is_updating_chrome_style { false };
    bool m_has_pending_chrome_style_update { false };
};

}
