/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/kmalloc.h>
#include <LibWebView/FileDownloader.h>
#include <LibWebView/Forward.h>
#include <LibWebView/Settings.h>
#include <UI/Qt/Menu.h>

#include <QPointer>
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
class SidebarFieldPanel;
class Tab;

enum class ToolbarArrangement : u8 {
    // One row above the page.
    Row,
    // Two rows in the open sidebar: the navigation buttons, then the address field. The toolbar's other buttons sit in
    // its sidebar footer, which the sidebar shows at its bottom.
    Sidebar,
};

// A window has one toolbar, which shows the tab that is selected. Each tab keeps its own address field, which the
// toolbar holds while that tab is shown.
class WindowToolbar final
    : public QWidget
    , public WebView::FileDownloaderObserver
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
    // The toolbar's height in either arrangement, whichever it is in now.
    int height_in_row() const;
    int height_in_sidebar() const;
    // While the sidebar opens or closes, the toolbar's contents fade out of one arrangement and into the other.
    void set_contents_opacity(qreal);

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
    void lay_out_as_row();
    void lay_out_in_sidebar();
    void create_field_panel();
    bool field_panel_should_expand() const;
    void update_field_panel();
    void raise_field_panel();
    void apply_contents_opacity();

    WebView::IsPrivate m_is_private { WebView::IsPrivate::No };
    QPointer<Tab> m_tab;
    // The tab whose history a back or forward button's menu lists, which stays the same while the menu is open.
    QPointer<Tab> m_history_menu_tab;

    ToolbarArrangement m_arrangement { ToolbarArrangement::Row };
    QWidget* m_toolbar { nullptr };
    QWidget* m_navigation_buttons { nullptr };
    QWidget* m_location_edit_container { nullptr };
    QWidget* m_sidebar_footer { nullptr };
    // In the sidebar, the field's container floats in this panel over the field's slot, and grows while it is edited.
    QPointer<SidebarFieldPanel> m_field_panel;
    QWidget* m_field_slot { nullptr };
    QWidget* m_suggestions_area { nullptr };
    bool m_window_controls_visible { false };
    int m_sidebar_inset { 0 };
    qreal m_contents_opacity { 1 };
    // NB: The fields of the tabs shown so far share this layout, which leaves room around them for the focus ring.
    QHBoxLayout* m_location_edit_layout { nullptr };
    QPointer<LocationEdit> m_location_edit;
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
