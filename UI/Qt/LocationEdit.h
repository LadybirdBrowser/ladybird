/*
 * Copyright (c) 2023, Cameron Youell <cameronyouell@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/OwnPtr.h>
#include <AK/Vector.h>
#include <LibWebView/Autocomplete.h>
#include <LibWebView/Settings.h>

#include <QIcon>
#include <QLineEdit>
#include <QString>

class QAction;
class QEvent;
class QTimer;

namespace Ladybird {

class Autocomplete;

class LocationEdit final
    : public QLineEdit
    , public WebView::SettingsObserver {
    Q_OBJECT

public:
    explicit LocationEdit(QWidget*);

    Optional<URL::URL const&> url() const { return m_url; }
    void set_url(Optional<URL::URL>);
    void set_loading(bool);
    void set_favicon(QIcon const&);

    bool url_is_hidden() const { return m_url_is_hidden; }
    void set_url_is_hidden(bool url_is_hidden) { m_url_is_hidden = url_is_hidden; }

private:
    virtual void changeEvent(QEvent* event) override;
    virtual void focusInEvent(QFocusEvent* event) override;
    virtual void focusOutEvent(QFocusEvent* event) override;
    virtual void keyPressEvent(QKeyEvent* event) override;

    virtual void search_engine_changed() override;

    void update_placeholder();
    void update_chrome_style();
    void update_location_icon();
    void update_loading_icon();
    void highlight_location();
    bool text_matches_current_url() const;

    int apply_inline_autocomplete(Vector<WebView::AutocompleteSuggestion> const&);
    bool apply_inline_autocomplete_suggestion_text(QString const& suggestion_text, QString const& query);
    void apply_inline_autocomplete_text(QString const& inline_text, QString const& query);
    void restore_query();
    QString current_query() const;
    void reset_autocomplete_state();

    Autocomplete* m_autocomplete { nullptr };
    QAction* m_leading_icon_action { nullptr };
    QTimer* m_loading_animation_timer { nullptr };

    Optional<URL::URL> m_url;
    QIcon m_favicon;
    bool m_url_is_hidden { false };
    bool m_is_loading { false };
    bool m_is_updating_chrome_style { false };
    int m_loading_animation_frame { 0 };

    bool m_is_applying_inline_autocomplete { false };
    bool m_should_suppress_inline_autocomplete_on_next_change { false };
    QString m_current_inline_autocomplete_suggestion;
    QString m_suppressed_inline_autocomplete_query;
};

}
