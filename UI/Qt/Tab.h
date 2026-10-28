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
#include <LibAudio/Audibility.h>
#include <LibWakeLock/DisplaySleepInhibitor.h>
#include <LibWebCommon/Page/PageId.h>
#include <LibWebCommon/Page/ScreenWakeLockState.h>
#include <LibWebView/FileDownloader.h>
#include <LibWebView/SessionStore.h>
#include <LibWebView/Settings.h>
#include <UI/Qt/BookmarksBar.h>
#include <UI/Qt/FindInPageWidget.h>
#include <UI/Qt/LocationEdit.h>
#include <UI/Qt/PermissionPromptPopover.h>
#include <UI/Qt/WebContentView.h>

#include <QBoxLayout>
#include <QElapsedTimer>
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

class BookmarkPopover;
class BrowserWindow;
enum class ChromeIcon;
class DownloadsButton;
class DownloadsPopover;
class ReloadButton;
class RestoreSessionCard;
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

// What a restored tab shows until it's selected: It has no view, and so no WebContent process, until then.
struct DeferredTab {
    WebView::SessionTabId session_tab_id { 0 };
    URL::URL url;
    QString title;
    QIcon favicon;
};

class Popover;
class SiteSettingsPopover;
class Tab final
    : public QWidget
    , public WebView::FileDownloaderObserver
    , public WebView::SettingsObserver {
    Q_OBJECT

public:
    AK_ALLOC_WITH_KMALLOC;

    Tab(BrowserWindow* window, Optional<WebView::CanonicalTraversable&> traversable = {});
    Tab(BrowserWindow* window, DeferredTab);
    virtual ~Tab() override;

    WebContentView& view()
    {
        VERIFY(m_view);
        return *m_view;
    }
    WebContentView const& view() const
    {
        VERIFY(m_view);
        return *m_view;
    }

    bool is_loaded() const { return m_view != nullptr; }
    // Builds the tab's view and toolbar, and restores its history, if it hasn't loaded yet.
    void ensure_loaded();

    URL::URL url() const;

    Optional<WebView::SessionTabId> session_tab_id() const;
    void set_session_tab_id(WebView::SessionTabId);
    void clear_session_tab_id();

    // A page that has never had a real size must be sized while hidden, so that it doesn't load into Qt's placeholder
    // geometry. Once it has had one, it keeps its size while hidden and takes the window's when it is shown again.
    bool has_had_real_size() const { return m_has_had_real_size; }
    void did_get_real_size() { m_has_had_real_size = true; }

    void navigate(URL::URL const&);
    void load_html(StringView);

    void open_file();

    void show_find_in_page();
    void show_restore_session_card(size_t tab_count, Function<void()> on_restore);
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
    // Whether the page being loaded has yet to respond.
    bool is_waiting_for_response() const { return m_is_loading && m_is_waiting_for_response; }
    // How strongly the tab's loading ring shows: fully while the tab loads, then fading out once it has loaded.
    qreal loading_ring_opacity() const;
    // How far the favicon has shrunk to sit inside the loading ring: it shrinks as the tab starts loading, and grows back
    // as the ring fades out.
    qreal loading_favicon_shrink() const;
    // A tab in the background whose title changes after it has loaded has news, until it is selected.
    bool needs_attention() const { return m_needs_attention; }
    void clear_attention() { m_needs_attention = false; }
    QString title() const;

    QMenu* context_menu() const { return m_context_menu; }

    QWidget* toolbar_container() const { return m_toolbar_container; }

    void set_vertical_tabs_enabled(bool);
    void set_window(BrowserWindow&);
    void set_toolbar_container_in_tab_layout(bool);
    void set_toolbar_window_controls_visible(bool);
    // While the sidebar is collapsed, the toolbar holds the Ladybird button and the sidebar's toggle, after the window
    // controls, here.
    QWidget& toolbar_sidebar_slot() { return *m_toolbar_sidebar_slot; }
    int toolbar_lead_beside_collapsed_sidebar(QWidget const& ladybird_button, QSize sidebar_toggle_size) const;
    void set_toolbar_leading_room(int);
    static int toolbar_height(bool window_controls_visible);
    void update_window_control_icons();
    void update_hover_label();

    void enter_report_review(Tab* tab_to_activate_on_close, Optional<QString> title = {});

    bool is_pinned() const { return m_is_pinned; }
    // BrowserWindow pins and unpins tabs, since it keeps each window's pinned tabs at the start of its strip.
    void set_pinned(bool);

    bool url_is_hidden() const { return m_location_edit->url_is_hidden(); }
    void set_url_is_hidden(bool url_is_hidden) { m_location_edit->set_url_is_hidden(url_is_hidden); }

public:
    enum class FocusRingTransition : u8 {
        FadeIn,
        Immediate,
    };
    void show_downloads_popover();
    void update_downloads_button();
    void show_private_session_popover();
    void show_site_settings_popover();
    void refresh_site_settings_popover();
    void show_zoom_popover();
    void show_bookmark_popover();
    void close_popovers();

public slots:
    void focus_location_editor(FocusRingTransition = FocusRingTransition::FadeIn);
    void park_location_editing();
    LocationEdit* location_edit_with_parked_editing() const { return is_loaded() && m_location_edit->has_parked_editing_session() ? m_location_edit : nullptr; }

signals:
    void title_changed(int id, QString const&);
    void favicon_changed(int id, QIcon const&);
    void audibility_changed(int id, Audio::Audibility);
    void loading_changed();
    void loaded();

private:
    void create_tab_shell();
    void create_tab_context_menu();
    void initialize(Optional<WebView::CanonicalTraversable&>);

    void location_edit_return_pressed(String, Optional<URL::URL>, WebView::OmniboxDestinationKind);
    virtual void resizeEvent(QResizeEvent*) override;
    virtual void hideEvent(QHideEvent*) override;
    virtual void showEvent(QShowEvent*) override;
    virtual bool event(QEvent*) override;
    virtual bool eventFilter(QObject*, QEvent*) override;

    virtual void tab_settings_changed() override;
    virtual void config_variable_changed(WebView::ConfigVariableID) override;

    void recreate_toolbar_icons();
    QToolButton* create_navigation_history_toolbar_button(QWidget& parent, QAction&, int direction);
    void update_chrome_style();
    void update_tab_title();
    void leave_report_review();
    void update_downloads_popover();
    void position_downloads_popover();
    void position_private_session_popover();
    void hide_hover_label();
    QImage render_hover_text(QString const&) const;
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
    QWidget* m_toolbar_sidebar_slot { nullptr };
    QWidget* m_toolbar_window_controls { nullptr };
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

    QPointer<DownloadsPopover> m_downloads_popover;
    QPointer<MessagePopover> m_private_session_popover;
    QPointer<SiteSettingsPopover> m_site_settings_popover;
    // The site the open popover shows, which its changes apply to whatever the tab shows by then.
    Optional<URL::Origin> m_site_settings_popover_origin;
    QTimer* m_content_blocking_update_timer { nullptr };
    QPointer<Popover> m_zoom_popover;
    QPointer<BookmarkPopover> m_bookmark_popover;
    QPointer<Tab> m_tab_to_activate_on_close;
    LocationEdit* m_location_edit { nullptr };
    WebContentView* m_view { nullptr };
    FindInPageWidget* m_find_in_page { nullptr };
    QPointer<RestoreSessionCard> m_restore_session_card;
    Optional<WakeLock::DisplaySleepInhibitor> m_screen_display_sleep_inhibitor;
    BrowserWindow* m_window { nullptr };
    QString m_title;
    Optional<QString> m_fixed_title;
    Optional<u64> m_report_review_navigation_listener;
    bool m_has_loaded_report_review_page { false };
    HyperlinkLabel* m_hover_label { nullptr };
    // The address of the hovered link, which shows over the bottom corner of the page that the pointer is not over.
    QString m_hover_text;
    bool m_hover_text_is_on_right { false };
    // Where the hover text shows over the page, in the view, when the view can stack it above the page itself.
    QRect m_hover_text_rect_over_page;
    QIcon m_favicon;
    bool m_is_loading { false };
    bool m_needs_attention { false };
    bool m_is_waiting_for_response { false };
    QElapsedTimer m_load_started_clock;
    QElapsedTimer m_load_finished_clock;
    bool m_has_had_real_size { false };
    bool m_is_updating_chrome_style { false };
    bool m_vertical_tabs_enabled { false };

    QMenu* m_context_menu { nullptr };
    QMenu* m_page_context_menu { nullptr };
    QMenu* m_link_context_menu { nullptr };
    QMenu* m_selected_text_link_context_menu { nullptr };
    QMenu* m_image_context_menu { nullptr };
    QMenu* m_media_context_menu { nullptr };
    QMenu* m_select_dropdown { nullptr };

    QAction* m_navigate_back_action { nullptr };
    QAction* m_navigate_forward_action { nullptr };
    QAction* m_reload_action { nullptr };
    QAction* m_reload_bypassing_cache_action { nullptr };
    QAction* m_stop_action { nullptr };
    ReloadButton* m_reload_button { nullptr };
    QAction* m_open_downloads_page_action { nullptr };
    QAction* m_pin_tab_action { nullptr };
    DownloadsButton* m_downloads_button { nullptr };
    size_t m_completed_download_count { 0 };

    JavaScriptDialog* m_javascript_dialog { nullptr };
    Function<void(bool)> m_before_unload_completion;
    QPointer<QColorDialog> m_color_picker_dialog;
    QPointer<QFileDialog> m_file_picker_dialog;
    QPointer<QMessageBox> m_external_url_confirmation_dialog;
    PermissionPromptQueue m_permission_prompts;

    bool m_suppress_javascript_dialogs_until_navigation { false };
    // The dialogs the current document has shown, and whether the user stopped it from showing more.
    u64 m_javascript_dialogs_shown_by_document { 0 };
    bool m_javascript_dialogs_blocked_for_document { false };
    bool m_already_requested_close { false };
    bool m_preserve_close_retry_state_on_dialog_dismissal { false };
    bool m_window_is_closing { false };
    bool m_is_pinned { false };

    bool m_toolbar_window_controls_visible { false };
    Optional<WebView::SessionTabId> m_session_tab_id;
    Optional<DeferredTab> m_deferred_tab;
};

}
