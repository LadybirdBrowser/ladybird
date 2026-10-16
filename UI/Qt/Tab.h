/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Matthew Costa <ucosty@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <AK/kmalloc.h>
#include <LibWakeLock/DisplaySleepInhibitor.h>
#include <LibWebCommon/HTML/AudioPlayState.h>
#include <LibWebCommon/Page/PageId.h>
#include <LibWebCommon/Page/ScreenWakeLockState.h>
#include <LibWebView/FileDownloader.h>
#include <LibWebView/Settings.h>
#include <UI/Qt/BookmarksBar.h>
#include <UI/Qt/FindInPageWidget.h>
#include <UI/Qt/LocationEdit.h>
#include <UI/Qt/WebContentView.h>

#include <QBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QToolButton>
#include <QWidget>

class QColorDialog;
class QFileDialog;
class QMessageBox;
class QTimer;
class QVariantAnimation;

namespace Ladybird {

class BrowserWindow;
enum class ChromeIcon;
class DownloadsPopover;
class ContentBlockingButton;
class ContentBlockingPopover;
class ReloadButton;
class JavaScriptDialog;
class MessagePopover;
class WindowControlButton;

class HyperlinkLabel final : public QLabel {
    Q_OBJECT

public:
    AK_ALLOC_WITH_KMALLOC;

    explicit HyperlinkLabel(QWidget* parent = nullptr)
        : QLabel(parent, Qt::ToolTip | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint)
    {
        setAttribute(Qt::WA_ShowWithoutActivating);
        setMouseTracking(true);
    }

    virtual void enterEvent(QEnterEvent* event) override
    {
        emit mouse_entered(event);
    }

signals:
    void mouse_entered(QEnterEvent*);
};

class Tab final
    : public QWidget
    , public WebView::FileDownloaderObserver
    , public WebView::SettingsObserver {
    Q_OBJECT

public:
    AK_ALLOC_WITH_KMALLOC;

    Tab(BrowserWindow* window, Optional<WebView::CanonicalTraversable&> traversable = {});
    virtual ~Tab() override;

    WebContentView& view() { return *m_view; }
    WebContentView const& view() const { return *m_view; }

    // A page that has never had a real size must be sized while hidden, so that it doesn't load into Qt's placeholder
    // geometry. Once it has had one, it keeps its size while hidden and takes the window's when it is shown again.
    bool has_had_real_size() const { return m_has_had_real_size; }
    void did_get_real_size() { m_has_had_real_size = true; }

    void navigate(URL::URL const&);
    void load_html(StringView);

    void open_file();

    void show_find_in_page();
    void find_previous();
    void find_next();

    BookmarksBar& bookmarks_bar() { return *m_bookmarks_bar; }
    bool has_bookmarks_bar_in_page() const;
    void update_bookmarks_bar_visibility();

    void request_close();
    void request_window_close_preflight(Function<void(bool)> on_complete);
    void cancel_window_close_preflight();
    Function<void()> prepare_for_window_close();

    QIcon const& favicon() const { return m_favicon; }
    QIcon tab_icon() const;
    bool is_loading() const { return m_is_loading; }
    QString title() const;

    QMenu* context_menu() const { return m_context_menu; }

    QToolButton* hamburger_button() const { return m_hamburger_button; }
    QWidget* toolbar_container() const { return m_toolbar_container; }

    void set_vertical_tabs_enabled(bool);
    void set_vertical_tabs_position(WebView::VerticalTabsPosition);
#ifdef AK_OS_MACOS
    void update_web_view_frame(bool chrome_visible, Optional<WebView::VerticalTabsPosition> sidebar_position);
#endif
    void set_window(BrowserWindow&);
    void set_toolbar_container_in_tab_layout(bool);
    void set_toolbar_window_controls_visible(bool);
    void update_window_control_icons();
    void update_hover_label();

    void enter_report_review(Tab* tab_to_activate_on_close, Optional<QString> title = {});

    bool is_pinned() const { return m_is_pinned; }
    // BrowserWindow pins and unpins tabs, since it keeps each window's pinned tabs at the start of its strip.
    void set_pinned(bool);

    bool url_is_hidden() const { return m_location_edit->url_is_hidden(); }
    void set_url_is_hidden(bool url_is_hidden) { m_location_edit->set_url_is_hidden(url_is_hidden); }

public slots:
    void focus_location_editor();
    void park_location_editing();
    LocationEdit* location_edit_with_parked_editing() const { return m_location_edit->has_parked_editing_session() ? m_location_edit : nullptr; }

signals:
    void title_changed(int id, QString const&);
    void favicon_changed(int id, QIcon const&);
    void audio_play_state_changed(int id, Web::HTML::AudioPlayState);
    void loading_changed();
    void loading_finished();

private:
    void location_edit_return_pressed(String, Optional<URL::URL>, WebView::OmniboxDestinationKind);
#ifdef AK_OS_MACOS
    virtual void paintEvent(QPaintEvent*) override;
#endif
    virtual void resizeEvent(QResizeEvent*) override;
    virtual void hideEvent(QHideEvent*) override;
    virtual bool event(QEvent*) override;
    virtual bool eventFilter(QObject*, QEvent*) override;

    virtual void tab_settings_changed() override;
    virtual void config_variable_changed(WebView::ConfigVariableID) override;
    virtual void content_blocker_count_visibility_changed() override;

    void recreate_toolbar_icons();
    void update_vertical_tabs_toolbar_button_placement();
    void connect_hamburger_menu();
    QToolButton* create_navigation_history_toolbar_button(QWidget& parent, QAction&, int direction);
    void update_chrome_style();
    void update_tab_title();
    void leave_report_review();
    void update_downloads_button();
    void update_downloads_popover();
    void show_downloads_popover();
    void position_downloads_popover();
    void show_content_blocking_popover();
    void update_content_blocking_controls(WebView::ContentBlockingSnapshot const&);
    void show_private_session_popover();
    void position_private_session_popover();
    void hide_hover_label();
    void set_loading(bool);
    void update_tab_icon();
    int tab_index();
    void set_screen_wake_lock_state(Web::ScreenWakeLockState);

    bool can_expand_bookmarks_bar_on_hover() const;
    bool cursor_is_over_bookmarks_bar_hover_region() const;
    void update_bookmarks_bar_hover();
    void set_bookmarks_bar_hover_expanded(bool);
    void update_bookmarks_bar_geometry();

    virtual void download_added(WebView::FileDownloader::Download const&) override;
    virtual void download_updated(WebView::FileDownloader::Download const&) override;
    virtual void download_removed(u64, WebView::IsPrivate) override;

    QWidget* m_toolbar_container { nullptr };
    QWidget* m_toolbar { nullptr };
    QWidget* m_performance_monitor { nullptr };
    QWidget* m_toolbar_window_controls_separator { nullptr };
    QWidget* m_toolbar_window_controls { nullptr };
    QSpacerItem* m_sidebar_toggle_navigation_spacer { nullptr };
    QToolButton* m_left_toggle_vertical_tabs_expanded_button { nullptr };
    QToolButton* m_right_toggle_vertical_tabs_expanded_button { nullptr };
    WindowControlButton* m_minimize_window_button { nullptr };
    WindowControlButton* m_maximize_window_button { nullptr };
    WindowControlButton* m_close_window_button { nullptr };

    BookmarksBar* m_bookmarks_bar { nullptr };
    QWidget* m_bookmarks_bar_container { nullptr };
    QTimer* m_bookmarks_bar_hover_expand_timer { nullptr };
    QTimer* m_bookmarks_bar_hover_collapse_timer { nullptr };
    QVariantAnimation* m_bookmarks_bar_hover_expand_animation { nullptr };
    bool m_bookmarks_bar_hover_expanded { false };
    qreal m_bookmarks_bar_hover_expand_progress { 0.0 };

    QPushButton* m_private_badge { nullptr };
    QToolButton* m_hamburger_button { nullptr };
    ContentBlockingButton* m_content_blocking_button { nullptr };
    QPointer<ContentBlockingPopover> m_content_blocking_popover;
    Optional<String> m_content_blocking_popover_reload_site;
    QToolButton* m_downloads_button { nullptr };
    QPointer<DownloadsPopover> m_downloads_popover;
    QPointer<MessagePopover> m_private_session_popover;
    QPointer<Tab> m_tab_to_activate_on_close;
    LocationEdit* m_location_edit { nullptr };
    WebContentView* m_view { nullptr };
    FindInPageWidget* m_find_in_page { nullptr };
    Optional<WakeLock::DisplaySleepInhibitor> m_screen_display_sleep_inhibitor;
    BrowserWindow* m_window { nullptr };
    QString m_title;
    Optional<QString> m_fixed_title;
    Optional<u64> m_report_review_navigation_listener;
    bool m_has_loaded_report_review_page { false };
    HyperlinkLabel* m_hover_label { nullptr };
    QIcon m_favicon;
    QTimer* m_loading_animation_timer { nullptr };
    bool m_is_loading { false };
    bool m_has_had_real_size { false };
    bool m_is_updating_chrome_style { false };
    bool m_vertical_tabs_enabled { false };
    WebView::VerticalTabsPosition m_vertical_tabs_position { WebView::VerticalTabsPosition::Left };
#ifdef AK_OS_MACOS
    QHBoxLayout* m_view_layout { nullptr };
#endif
    int m_loading_animation_frame { 0 };

    QMenu* m_context_menu { nullptr };
    QMenu* m_page_context_menu { nullptr };
    QMenu* m_link_context_menu { nullptr };
    QMenu* m_selected_text_link_context_menu { nullptr };
    QMenu* m_image_context_menu { nullptr };
    QMenu* m_media_context_menu { nullptr };
    QMenu* m_select_dropdown { nullptr };

    QAction* m_toggle_vertical_tabs_expanded_action { nullptr };
    QAction* m_navigate_back_action { nullptr };
    QAction* m_navigate_forward_action { nullptr };
    QAction* m_reload_action { nullptr };
    QAction* m_stop_action { nullptr };
    ReloadButton* m_reload_button { nullptr };
    QAction* m_open_downloads_page_action { nullptr };
    QAction* m_pin_tab_action { nullptr };
    Optional<ChromeIcon> m_downloads_button_icon;
    QString m_downloads_button_tooltip;

    JavaScriptDialog* m_javascript_dialog { nullptr };
    Function<void(bool)> m_before_unload_completion;
    QPointer<QColorDialog> m_color_picker_dialog;
    QPointer<QFileDialog> m_file_picker_dialog;
    QPointer<QMessageBox> m_external_url_confirmation_dialog;
    // Requests the same dialog would answer wait on the one open, so a page cannot stack dialogs by asking again.
    struct PendingPermissionRequests {
        QPointer<QMessageBox> dialog;
        Vector<Function<void(bool granted)>> on_decided;
    };
    HashMap<String, PendingPermissionRequests> m_pending_permission_requests;

    bool m_suppress_javascript_dialogs_until_navigation { false };
    bool m_already_requested_close { false };
    bool m_preserve_close_retry_state_on_dialog_dismissal { false };
    bool m_window_is_closing { false };
    bool m_is_pinned { false };
};

}
