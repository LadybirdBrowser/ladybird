/*
 * Copyright (c) 2023, Cameron Youell <cameronyouell@gmail.com>
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibWebView/Autocomplete.h>

#include <QObject>
#include <QPointer>

class QFrame;
class QLineEdit;
class QListView;
class QMouseEvent;
class QWidget;

namespace Ladybird {

class AutocompleteModel;
class AutocompleteDelegate;

class Autocomplete final : public QObject {
    Q_OBJECT

public:
    explicit Autocomplete(QLineEdit* anchor);
    virtual ~Autocomplete() override;

    AK::Function<void(Vector<WebView::AutocompleteSuggestion>, WebView::AutocompleteResultKind)> on_query_complete;

    void query_autocomplete_engine(String);
    void cancel_pending_query();

    void show_with_suggestions(Vector<WebView::AutocompleteSuggestion>, int selected_suggestion_index);
    bool close();
    bool is_visible() const;

    void clear_selection();
    Optional<String> selected_suggestion() const;
    bool select_next_suggestion();
    bool select_previous_suggestion();

signals:
    void suggestion_activated(QString);
    void suggestion_highlighted(QString);
    void did_close();

protected:
    virtual bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QWidget* replay_target_at(QPoint const& global_position) const;
    void update_hover_replay_target(QWidget* target);
    bool replay_outside_mouse_event(QMouseEvent const& event);
    void position_popup();
    bool is_selectable_row(int row) const;
    int step_to_selectable_row(int from, int direction) const;
    void select_row(int row, bool notify = true);

    QLineEdit* m_anchor { nullptr };
    QFrame* m_popup { nullptr };
    QListView* m_list_view { nullptr };
    AutocompleteModel* m_model { nullptr };
    AutocompleteDelegate* m_delegate { nullptr };
    QPointer<QWidget> m_hover_replay_target;
    QPointer<QWidget> m_pressed_replay_target;

    NonnullOwnPtr<WebView::Autocomplete> m_autocomplete;
};

}
