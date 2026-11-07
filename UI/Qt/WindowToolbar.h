/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/kmalloc.h>
#include <LibWebView/FileDownloader.h>
#include <LibWebView/Forward.h>
#include <LibWebView/Settings.h>
#include <UI/Qt/Menu.h>
#include <UI/Qt/RemoteDownloads.h>

#include <QPointer>
#include <QRect>
#include <QWidget>

class QAction;
class QHBoxLayout;
class QToolButton;

namespace Ladybird {

class BrowserWindow;
class DownloadsButton;
class DownloadsPopover;
class FeedbackButton;
class FeedbackPopover;
class LocationEdit;
class PerformanceMonitorWidget;
class Popover;
class ReloadButton;
class FieldPanel;
class Tab;

enum class ToolbarArrangement : u8 {
    // One row above the page.
    Row,
    // In the open sidebar: the toolbar holds the address field, and the sidebar shows the navigation buttons in its top
    // row and the toolbar's other buttons in the sidebar footer at its bottom.
    Sidebar,
};

// A window has one toolbar, which shows the tab that is selected. Each tab keeps its own address field, which the
// toolbar holds while that tab is shown.
class WindowToolbar final
    : public QWidget
    , public RemoteDownloadsObserver
    , public WebView::SettingsObserver {
    Q_OBJECT

public:
    AK_ALLOC_WITH_KMALLOC;

    WindowToolbar(WebView::IsPrivate, QWidget* parent);
    virtual ~WindowToolbar() override;

    static int toolbar_height(bool window_controls_visible);

    void bind(Tab*);
    void release_location_edit(Tab&);

    // While the sidebar is collapsed, the toolbar holds the Ladybird button and the sidebar's toggle, after the window
    // controls, here.
    QWidget& sidebar_slot() { return *m_sidebar_slot; }
    void set_window_controls_visible(bool);
    int lead_beside_collapsed_sidebar(QWidget const& ladybird_buttons, QSize sidebar_toggle_size) const;
    void set_leading_room(int);
    void update_window_control_icons();
    void set_vertical_tabs_enabled(bool);
    void set_bookmarks_bar_below(bool);

    ToolbarArrangement arrangement() const { return m_arrangement; }
    // The sidebar's own margin plus the given inset keeps the toolbar's contents clear of the sidebar's edges.
    void set_arrangement(ToolbarArrangement, int sidebar_inset = 0);
    QWidget& sidebar_footer() { return *m_sidebar_footer; }
    QWidget& navigation_buttons() { return *m_navigation_buttons; }
    static int navigation_buttons_width_in_sidebar();
    static int sidebar_footer_height();
    // What lies over the start of the sidebar footer, which its buttons keep clear of.
    void set_sidebar_footer_leading_room(int);
    // The row of its own that the tab performance monitor shows in, which ends the page column in every layout.
    QWidget& performance_monitor_row() { return *m_performance_monitor_row; }
    // The toolbar's height in either arrangement, whichever it is in now.
    int height_in_row() const;
    int height_in_sidebar() const;
    // While the sidebar opens or closes, the toolbar's contents fade out of one arrangement and into the other.
    void set_contents_opacity(qreal);
    // While the sidebar opens or closes, the field's panel shows only within this part of the window, as the page and the
    // sidebar slide under it.
    void set_field_clip(Optional<QRect>);

    void show_downloads_popover();
    void update_downloads_button();
    void update_feedback_button();

private:
    virtual bool event(QEvent*) override;
    virtual bool eventFilter(QObject*, QEvent*) override;

    virtual void tab_settings_changed() override;
    virtual void config_variable_changed(WebView::ConfigVariableID) override;

    virtual void download_added(WebView::FileDownloader::Download const&) override;
    virtual void download_updated(WebView::FileDownloader::Download const&) override;
    virtual void download_removed(u64, WebView::IsPrivate) override;

    BrowserWindow& browser_window() const;
    void show_tab(Tab&);
    void show_location_edit(LocationEdit*);
    QToolButton* create_navigation_history_toolbar_button(QWidget& parent, QAction&, int direction);
    void recreate_icons();
    void update_chrome_style();
    void update_downloads_popover();
    void position_downloads_popover();
    void show_feedback_popover();
    void fit_feedback_popover();
    void move_popover_to_button(Popover&, QWidget& button);
    void size_navigation_buttons(QSize, int spacing);
    void lay_out_as_row();
    void lay_out_in_sidebar();
    void create_field_panel();
    QWidget& field_slot();
    QMargins field_margins_at_rest() const;
    void host_field_in_panel();
    bool field_panel_should_expand() const;
    void update_field_panel();
    void raise_field_panel();
    void apply_contents_opacity();
    void update_performance_monitor_row();

    WebView::IsPrivate m_is_private { WebView::IsPrivate::No };
    QPointer<Tab> m_tab;
    // The tab whose history a back or forward button's menu lists, which stays the same while the menu is open.
    QPointer<Tab> m_history_menu_tab;

    ToolbarArrangement m_arrangement { ToolbarArrangement::Row };
    QWidget* m_toolbar { nullptr };
    QWidget* m_navigation_buttons { nullptr };
    QWidget* m_location_edit_container { nullptr };
    QWidget* m_sidebar_footer { nullptr };
    // The field's container floats in this panel over the field's slot, and grows while it lists suggestions.
    QPointer<FieldPanel> m_field_panel;
    QWidget* m_field_slot { nullptr };
    QWidget* m_suggestions_area { nullptr };
    bool m_window_controls_visible { false };
    int m_sidebar_inset { 0 };
    int m_sidebar_footer_leading_room { 0 };
    qreal m_contents_opacity { 1 };
    Optional<QRect> m_field_clip;
    // NB: The fields of the tabs shown so far share this layout, which leaves room around them for the focus ring.
    QHBoxLayout* m_location_edit_layout { nullptr };
    QPointer<LocationEdit> m_location_edit;
    QWidget* m_performance_monitor_row { nullptr };
    PerformanceMonitorWidget* m_performance_monitor { nullptr };
    QWidget* m_window_controls_separator { nullptr };
    QWidget* m_sidebar_slot { nullptr };
    QWidget* m_window_controls { nullptr };
    QToolButton* m_minimize_window_button { nullptr };
    QToolButton* m_maximize_window_button { nullptr };
    QToolButton* m_close_window_button { nullptr };

    ActionBinding m_navigate_back_action { *this, IncludeActionIcon::No };
    ActionBinding m_navigate_forward_action { *this, IncludeActionIcon::No };
    QAction* m_reload_action { nullptr };
    QAction* m_reload_bypassing_cache_action { nullptr };
    QAction* m_stop_action { nullptr };
    QAction* m_open_downloads_page_action { nullptr };
    ReloadButton* m_reload_button { nullptr };

    DownloadsButton* m_downloads_button { nullptr };
    QPointer<DownloadsPopover> m_downloads_popover;
    size_t m_completed_download_count { 0 };

    FeedbackButton* m_feedback_button { nullptr };
    QPointer<FeedbackPopover> m_feedback_popover;

    bool m_is_updating_chrome_style { false };
};

}
