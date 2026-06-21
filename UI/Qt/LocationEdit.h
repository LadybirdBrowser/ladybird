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

#include <QLineEdit>
#include <QString>

class QAction;
class QEvent;
class QGraphicsDropShadowEffect;
class QMouseEvent;
class QResizeEvent;
class QToolButton;
class QVariantAnimation;

namespace Ladybird {

class Autocomplete;

class LocationEdit final
    : public QLineEdit
    , public WebView::SettingsObserver {
    Q_OBJECT

public:
    explicit LocationEdit(QWidget*);

    void set_trailing_action(QAction*);
    QAction* trailing_action() const;
    void set_zoom_action(QAction*);

    Optional<URL::URL const&> url() const { return m_url; }
    void set_url(Optional<URL::URL>);
    bool url_is_hidden() const { return m_url_is_hidden; }
    void set_url_is_hidden(bool);
    void show_autocomplete();

private:
    virtual void changeEvent(QEvent* event) override;
    virtual void focusInEvent(QFocusEvent* event) override;
    virtual void focusOutEvent(QFocusEvent* event) override;
    virtual void keyPressEvent(QKeyEvent* event) override;
    virtual void mouseReleaseEvent(QMouseEvent* event) override;
    virtual void resizeEvent(QResizeEvent* event) override;

    virtual void search_engine_changed() override;

    void show_full_url_preserving_display_selection();
    int serialized_url_position_for_display_position(int) const;
    void update_text_margins();
    int trailing_text_margin() const;
    void update_trailing_item_positions();
    void update_placeholder();
    void update_chrome_style();
    void update_location_icon();
    void update_zoom_indicator();
    void update_focus_glow(int alpha);
    void schedule_chrome_style_update();
    void animate_focus_glow(int target_alpha);
    void highlight_location();
    bool text_matches_current_url() const;
    QString serialized_url() const;
    QString display_url() const;

    int apply_inline_autocomplete(Vector<WebView::AutocompleteSuggestion> const&);
    bool apply_inline_autocomplete_suggestion_text(QString const& suggestion_text, QString const& query, bool allow_preview = false);
    void apply_inline_autocomplete_text(QString const& inline_text, QString const& query);
    void apply_autocomplete_preview_text(QString const& suggestion_text, QString const& query);
    void activate_selected_autocomplete_suggestion();
    void restore_query();
    void set_text_without_inline_autocomplete(QString const& text);
    bool should_restore_autocomplete_query() const;
    QString autocomplete_query() const;
    QString current_query() const;
    void reset_autocomplete_state();

    Autocomplete* m_autocomplete { nullptr };
    QToolButton* m_leading_icon_button { nullptr };
    QToolButton* m_trailing_action_button { nullptr };
    QToolButton* m_zoom_indicator_button { nullptr };
    QVariantAnimation* m_focus_glow_animation { nullptr };
    QGraphicsDropShadowEffect* m_focus_glow_effect { nullptr };
    QAction* m_zoom_action { nullptr };

    Optional<URL::URL> m_url;
    bool m_url_is_hidden { false };
    bool m_has_user_edited_hidden_url { false };
    bool m_is_updating_chrome_style { false };
    bool m_has_pending_chrome_style_update { false };
    bool m_should_show_full_url_on_mouse_release { false };
    int m_text_leading_margin { 0 };
    int m_focus_glow_alpha { 0 };

    bool m_is_applying_inline_autocomplete { false };
    bool m_should_suppress_inline_autocomplete_on_next_change { false };
    bool m_has_highlighted_autocomplete_suggestion { false };
    bool m_should_preserve_inline_autocomplete_on_close { false };
    bool m_should_skip_autocomplete_cancel_on_focus_out { false };
    QString m_autocomplete_popup_query;
    QString m_autocomplete_query_without_inline;
    QString m_autocomplete_preview_query;
    QString m_current_inline_autocomplete_suggestion;
    QString m_pending_autocomplete_activation_query;
    QString m_suppressed_inline_autocomplete_query;
};

}
