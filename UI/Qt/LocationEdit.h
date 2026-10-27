/*
 * Copyright (c) 2023, Cameron Youell <cameronyouell@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/kmalloc.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/Omnibox.h>
#include <LibWebView/Settings.h>

#include <QPointer>
#include <QString>
#include <UI/Qt/NativeTextEditingLineEdit.h>

class QAction;
class QEvent;
class QGraphicsEffect;
class QGraphicsOpacityEffect;
class QLabel;
class QMouseEvent;
class QResizeEvent;
class QTimer;
class QToolButton;
class QVariantAnimation;

namespace Ladybird {

class Autocomplete;

class LocationEdit final
    : public NativeTextEditingLineEdit
    , public WebView::SettingsObserver {
    Q_OBJECT

public:
    AK_ALLOC_WITH_KMALLOC;

    LocationEdit(QWidget*, WebView::IsPrivate);

    void set_trailing_action(QAction*);
    // The content blocker's button stands in the field, before the bookmark star.
    QAction* trailing_action() const;
    // The star pops as the page is bookmarked, and its fill fades as the bookmark is removed. It moves only for a change
    // the user makes, rather than as another page is shown, so such a change is made through here.
    enum class TrailingActionMotion : u8 {
        Pop,
        Fade,
    };
    void animate_trailing_action_change(TrailingActionMotion, Function<void()> const& change);
    // The knobs of the site settings icon slide past each other while the given popover is open.
    void follow_site_settings_popover(QWidget&);
    void set_zoom_action(QAction*);
    // Shows, while the page uses the user's location, a chip that says so.
    void set_location_in_use(bool);

    Optional<URL::URL const&> url() const { return m_url; }
    void set_url(Optional<URL::URL>);
    bool url_is_hidden() const { return m_url_is_hidden; }
    void set_url_is_hidden(bool);
    void show_autocomplete();
    void park_editing_session();
    bool has_parked_editing_session() const { return m_parked_editing_session.has_value(); }
    // Shows the focus ring of a focused field at once, rather than fading it in, as the focus moves to it from another
    // tab's field, whose ring was showing.
    void show_focus_ring_at_once();

    // Asks for the site settings of the page whose address is shown.
    Function<void()> on_site_settings_requested;
    // Asks for the page's zoom controls, shown when the zoom level in the field is clicked.
    Function<void()> on_zoom_requested;
    QWidget& trailing_action_button() const;
    QWidget& site_settings_button() const;
    Function<void(String, Optional<URL::URL>, WebView::OmniboxDestinationKind)> on_navigation;

signals:
    void focus_return_requested();

private:
    virtual void changeEvent(QEvent* event) override;
    virtual bool event(QEvent* event) override;
    virtual bool eventFilter(QObject* watched, QEvent* event) override;
    virtual void contextMenuEvent(QContextMenuEvent* event) override;
    virtual void focusInEvent(QFocusEvent* event) override;
    virtual void focusOutEvent(QFocusEvent* event) override;
    virtual void keyPressEvent(QKeyEvent* event) override;
    virtual void native_text_editing_command_will_execute(Web::TextEditingCommand const&) override;
    virtual void mouseReleaseEvent(QMouseEvent* event) override;
    virtual void resizeEvent(QResizeEvent* event) override;

    virtual void search_engine_settings_changed() override;

    void show_full_url_preserving_display_selection();
    int serialized_url_position_for_display_position(int) const;
    void update_text_margins();
    int trailing_text_margin() const;
    void update_trailing_item_positions();
    void update_chrome_style();
    void update_location_icon();
    void update_zoom_indicator();
    void update_location_in_use_chip();
    void update_focus_ring(qreal opacity);
    void set_field_hovered(bool);
    void schedule_chrome_style_update();
    void show_focus_ring();
    void hide_focus_ring();
    void update_trailing_action_visibility();
    void slide_site_settings_knobs(qreal travel);
    void update_completion_title();
    void highlight_location();
    bool text_matches_current_url() const;
    QString serialized_url() const;
    QString display_url() const;
    QString current_query() const;
    void set_text_and_restart_editing(QString const& text);

    WebView::Omnibox m_omnibox;
    Autocomplete* m_autocomplete { nullptr };
    QToolButton* m_leading_icon_button { nullptr };
    bool m_shows_site_settings_button { false };
    QPointer<QWidget> m_site_settings_popover;
    QVariantAnimation* m_site_settings_knob_animation { nullptr };
    qreal m_site_settings_knob_travel { 0 };
    QToolButton* m_trailing_action_button { nullptr };
    QToolButton* m_zoom_indicator_button { nullptr };
    QGraphicsOpacityEffect* m_zoom_indicator_opacity { nullptr };
    QVariantAnimation* m_zoom_indicator_fade { nullptr };
    QToolButton* m_location_in_use_button { nullptr };
    QTimer* m_location_in_use_collapse_timer { nullptr };
    QLabel* m_completion_title { nullptr };
    QVariantAnimation* m_focus_ring_animation { nullptr };
    QGraphicsEffect* m_focus_ring_effect { nullptr };
    QAction* m_zoom_action { nullptr };

    Optional<URL::URL> m_url;
    bool m_url_is_hidden { false };
    bool m_is_editing_location { false };

    struct ParkedEditingSession {
        int cursor_position { 0 };
        int selection_start { -1 };
        int selection_length { 0 };
    };
    Optional<ParkedEditingSession> m_parked_editing_session;
    bool m_has_user_edited_hidden_url { false };
    bool m_is_updating_chrome_style { false };
    bool m_is_updating_field_hover { false };
    bool m_has_pending_chrome_style_update { false };
    bool m_should_show_full_url_on_mouse_release { false };
    int m_text_leading_margin { 0 };
    qreal m_focus_ring_opacity { 0 };
};

}
