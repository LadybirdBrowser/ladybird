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

class QFrame;
class QLineEdit;
class QListView;
class QVariantAnimation;

namespace Ladybird {

class AutocompleteModel;
class AutocompleteDelegate;

// The suggestion popup below the location bar. This is a pure view: LocationEdit feeds it rows and a
// selection from the WebView::Omnibox model, and it reports clicks and outside-click
// dismissals back.
class Autocomplete final : public QObject {
    Q_OBJECT

public:
    AK_ALLOC_WITH_KMALLOC;

    explicit Autocomplete(QLineEdit* anchor);
    virtual ~Autocomplete() override;

    void update_chrome_style();
    void schedule_chrome_style_update();

    // While a host is set, the suggestions are listed at the end of the host's layout instead of in a popup of their own.
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
    void create_popup();
    QListView* create_list_view(QWidget& parent);
    QListView* list_view() const;
    int list_height() const;
    void show_current_suggestions();
    void set_anchor_popup_open(bool);
    bool is_visible() const;
    void position_popup();
    void cast_card_shadow();
    bool is_selectable_row(int row) const;

    QLineEdit* m_anchor { nullptr };
    QFrame* m_popup { nullptr };
    // The card's shadow, in a window of its own that lets the mouse through. Not used on macOS.
    QPointer<QWidget> m_shadow;
    QListView* m_list_view { nullptr };
    QPointer<QWidget> m_host;
    QPointer<QListView> m_hosted_list_view;
    AutocompleteModel* m_model { nullptr };
    AutocompleteDelegate* m_delegate { nullptr };
    bool m_is_updating_chrome_style { false };
    bool m_has_pending_chrome_style_update { false };
    // The panel slides down this far into place as it opens.
    qreal m_opening_offset { 0 };
    QVariantAnimation* m_opening_animation { nullptr };
};

}
