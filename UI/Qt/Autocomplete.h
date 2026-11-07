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
#include <AK/kmalloc.h>
#include <LibWebView/Autocomplete.h>

#include <QObject>
#include <QPointer>

class QLineEdit;
class QListView;

namespace Ladybird {

class AutocompleteModel;
class AutocompleteDelegate;

// The suggestions for the address field, listed below the field in its surface, which hosts the list. This is a pure
// view: LocationEdit feeds it rows and a selection from the WebView::Omnibox model, and it reports clicks and
// outside-click dismissals back.
class Autocomplete final : public QObject {
    Q_OBJECT

public:
    AK_ALLOC_WITH_KMALLOC;

    explicit Autocomplete(QLineEdit* anchor);
    virtual ~Autocomplete() override;

    void update_chrome_style();
    void schedule_chrome_style_update();

    // The suggestions are listed at the end of the host's layout. Without a host, there are none to see.
    void set_host(QWidget* host);

    void show_with_suggestions(Vector<WebView::AutocompleteSuggestion>, Optional<size_t> selected_suggestion_index);
    void set_selected_suggestion(Optional<size_t> suggestion_index);
    bool close();

signals:
    void suggestion_clicked(int suggestion_index);
    void dismissed();

protected:
    virtual bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QListView* create_list_view(QWidget& parent);
    int list_height() const;
    void show_current_suggestions();
    void set_anchor_popup_open(bool);
    bool is_visible() const;
    bool is_selectable_row(int row) const;

    QLineEdit* m_anchor { nullptr };
    QPointer<QWidget> m_host;
    QPointer<QListView> m_list_view;
    AutocompleteModel* m_model { nullptr };
    AutocompleteDelegate* m_delegate { nullptr };
    bool m_is_updating_chrome_style { false };
    bool m_has_pending_chrome_style_update { false };
};

}
