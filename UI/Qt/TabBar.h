/*
 * Copyright (c) 2024-2026, Tim Flynn <trflynn89@ladybird.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/Optional.h>
#include <AK/TypeCasts.h>
#include <AK/Vector.h>
#include <AK/kmalloc.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/Settings.h>
#include <UI/Qt/Tab.h>

#include <QHash>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QStackedWidget>
#include <QStaticText>
#include <QTabBar>
#include <QWidget>

class QAction;
class QBoxLayout;
class QContextMenuEvent;
class QDragEnterEvent;
class QDragLeaveEvent;
class QDragMoveEvent;
class QDropEvent;
class QEvent;
class QIcon;
class QLabel;
class QMouseEvent;
class QPaintEvent;
class QResizeEvent;
class QSpacerItem;
class QToolButton;
class QTimer;
class QVariantAnimation;
class QWheelEvent;

namespace Ladybird {

class AllTabsPopover;
class LadybirdButton;

class TabPreviewPopup;
class TabWidget;

static constexpr int TAB_ICON_SIZE = 16;

// How much further the first and last horizontal tabs reach toward the ends of the strip, to make room for the selected
// tab's foot there.
struct HorizontalTabEndRoom {
    qreal left { 0 };
    qreal right { 0 };
};

enum class TabLayout : u8 {
    Horizontal,
    VerticalCollapsed,
    VerticalExpanded,
};

class TabBar final : public QTabBar {
    Q_OBJECT

public:
    AK_ALLOC_WITH_KMALLOC;

    explicit TabBar(TabWidget*);

    void set_available_width(int width);

    TabWidget* tab_widget() const { return m_tab_widget; }
    TabLayout tab_layout() const { return m_tab_layout; }
    void set_tab_layout(TabLayout);
    // While collapsed vertical tabs only peek open on hover, the pinned tabs keep their rows, one tab each, rather than
    // forming the grid, so no tab moves out from under the pointer.
    void set_pinned_tabs_in_rows(bool);
    void set_vertical_tabs_position(WebView::VerticalTabsPosition);
    void refresh_tab_layout();
    // After a tab is closed with a click, the tabs keep their width until the pointer leaves the tab bar, so the next
    // close button lands under the pointer.
    void lock_tab_widths();
    void recreate_icons();
    int pinned_tab_count() const;

    void update_loading_ring_animation();
    // How far the end of the horizontal tabs is from where it settles, while tabs open and close.
    int horizontal_tabs_end_offset() const;
    // A horizontal tab that closes shrinks away as the others close the gap.
    void prepare_for_tab_removal(int index);

    int drop_index_at(QPoint const&) const;
    void update_drop_preview(QPoint const&);
    void hide_drop_preview();

private:
    virtual QSize sizeHint() const override;
    virtual QSize minimumSizeHint() const override;
    virtual QSize tabSizeHint(int index) const override;

    virtual void resizeEvent(QResizeEvent*) override;
    virtual void tabLayoutChange() override;

    virtual bool event(QEvent*) override;
    virtual void paintEvent(QPaintEvent*) override;
    virtual void contextMenuEvent(QContextMenuEvent* event) override;
    virtual void dragEnterEvent(QDragEnterEvent*) override;
    virtual void dragLeaveEvent(QDragLeaveEvent*) override;
    virtual void dragMoveEvent(QDragMoveEvent*) override;
    virtual void dropEvent(QDropEvent*) override;
    virtual bool eventFilter(QObject*, QEvent*) override;
    virtual void enterEvent(QEnterEvent*) override;
    virtual void leaveEvent(QEvent*) override;
    virtual void mouseDoubleClickEvent(QMouseEvent*) override;
    virtual void mousePressEvent(QMouseEvent*) override;
    virtual void mouseMoveEvent(QMouseEvent*) override;
    virtual void mouseReleaseEvent(QMouseEvent*) override;
    virtual void wheelEvent(QWheelEvent*) override;

    // How far a tab moves, and how much it grows, while tabs make way for a dragged tab.
    struct TabShift {
        qreal x_offset { 0 };
        qreal y_offset { 0 };
        qreal width_delta { 0 };
        qreal height_delta { 0 };
    };
    struct DropPreview {
        Vector<TabShift> tab_shifts;
        qreal strip_growth { 0 };
        qreal pinned_region_growth { 0 };
    };

    Optional<int> dragged_tab_index() const;
    bool dragging_pinned_tab() const;
    QRect incoming_drop_layout_rect(int position) const;
    int drop_slot_at(QPoint const&) const;
    void set_drop_slot(Optional<int>);
    void clear_drop_preview();
    DropPreview drop_preview_for_slot(Optional<int>) const;
    DropPreview current_drop_preview() const;
    TabShift current_tab_shift(int index) const;
    qreal current_strip_growth() const;
    qreal current_pinned_region_growth() const;
    QRect shifted_tab_rect(int index) const;
    qreal selection_of(int index) const;
    bool is_growing_horizontal_tab(int index, QRect const& tab_rect) const;
    QRectF horizontal_tab_contents_rect(int index, QRect const& tab_rect) const;
    QRect tab_drag_card_rect(int index) const;
    QPixmap render_tab_drag_pixmap(int index) const;
    void toggle_window_maximized();
    bool start_window_move();
    void start_tab_drag(int index);
    QPoint tab_drag_hot_spot() const;
    void pointer_left();
    void start_strip_drag(int index);
    void update_strip_drag(QPoint position);
    void end_strip_drag();
    void drag_tab_loose(Tab&, QPixmap card);
    void drag_tab_in_own_window(Tab&);
    void start_hover_animation(int tab_index, qreal target_progress);
    void update_layout_animation();
    QRect animated_tab_rect(int index, QRect const& target) const;
    HorizontalTabEndRoom horizontal_tab_end_room(int index) const;
    void schedule_tab_preview(int index);
    void show_tab_preview();
    void hide_tab_preview();
    QPoint tab_preview_position_for(int index, QSize const& popup_size) const;

    // Where the pinned tabs sit in vertical tabs: a grid of tiles when expanded, one icon column when collapsed, and a
    // separator below. Its height is 0 when there's no pinned tab.
    struct PinnedRegion {
        int count { 0 };
        int columns { 1 };
        int tile_width { 0 };
        int height { 0 };
        // While the sidebar opens or closes, the tiles move between their grid in the open sidebar, at 0, and the
        // collapsed sidebar's one column, at 1, whose rows are this wide.
        qreal collapsedness { 0 };
        int collapsed_row_width { 0 };
        // The pinned tabs stay in place only while they fit in half the bar's height. Past that, they scroll with the
        // unpinned tabs, so every tab stays within reach.
        bool scrolls { false };

        int fixed_height() const { return scrolls ? 0 : height; }
    };
    PinnedRegion pinned_region() const;
    PinnedRegion pinned_region_for(int pinned_count) const;
    QRect vertical_tab_rect(int index, PinnedRegion const&) const;
    bool pinned_tabs_are_tiles() const;
    int vertical_bar_width() const;
    int vertical_tabs_left() const;

    QRect visual_tab_rect(int index) const;
    int tab_index_at(QPoint const&) const;
    QSize vertical_size_hint(int tab_count) const;
    bool vertical_tabs_are_on_right() const;

    int max_scroll_offset() const;
    void set_scroll_offset(int);

    void set_hovered_tab_index(int);
    qreal loading_ring_opacity(int index) const;
    void ensure_tab_visible(int index);
    void update_tab_button_geometry();

    QPointer<TabWidget> m_tab_widget;

    TabLayout m_tab_layout { TabLayout::Horizontal };
    bool m_pinned_tabs_in_rows { false };
    WebView::VerticalTabsPosition m_vertical_tabs_position { WebView::VerticalTabsPosition::Left };
    int m_available_width { 0 };
    Optional<int> m_locked_tab_width;
    QTimer* m_tab_width_unlock_timer { nullptr };
    int m_hovered_tab_index { -1 };
    int m_hover_animation_tab_index { -1 };
    int m_scroll_offset { 0 };
    qreal m_hover_progress { 0.0 };
    QVariantAnimation* m_hover_animation { nullptr };

    // When another tab is selected, the selected tab's look cross-fades from the one before it.
    QPointer<Tab> m_previously_selected_tab;
    qreal m_selection_progress { 1.0 };
    QVariantAnimation* m_selection_animation { nullptr };

    Optional<int> m_drop_slot;
    DropPreview m_drop_preview_start;
    DropPreview m_drop_preview_target;
    qreal m_drop_preview_progress { 1.0 };
    QVariantAnimation* m_drop_preview_animation { nullptr };

    // When tabs open or close, each one moves from where it was drawn to its new place, and a new one grows into place.
    QVariantAnimation* m_layout_animation { nullptr };
    qreal m_layout_animation_progress { 1.0 };
    // The image of a closing tab, and where it was in the tab bar before any scrolling.
    struct ClosingTab {
        QPixmap image;
        QRect rect;
    };
    Optional<ClosingTab> m_closing_tab;
    // The tab painted as if it were not selected, while a picture of it is taken as it closes.
    int m_tab_painted_unselected { -1 };
    QRect closing_tab_rect() const;
    HashMap<Tab const*, QRect> m_layout_animation_start_rects;
    HashMap<Tab const*, HorizontalTabEndRoom> m_layout_animation_start_end_rooms;
    HashMap<Tab const*, QRect> m_settled_tab_rects;
    HashMap<Tab const*, HorizontalTabEndRoom> m_settled_tab_end_rooms;
    int m_settled_tab_count { 0 };

    QVariantAnimation* m_loading_ring_animation { nullptr };
    QIcon m_fallback_tab_icon;
    QHash<QString, QStaticText> m_tab_title_texts;
    // The tabs whose titles were too long to show in full when last painted.
    HashTable<Tab const*> m_tabs_with_cut_titles;
    QFont m_tab_title_texts_font;
    QPointer<Tab> m_pressed_tab;
    QPoint m_position_in_selected_tab_while_dragging;
    QPoint m_drag_start_position;
    // NB: A tab is dragged within its strip without a drag and drop session. It is drawn as a card that slides along the
    //     strip, until it is pulled out of the strip.
    QWidget* m_strip_drag_card { nullptr };
    QPixmap m_tab_drag_pixmap;
    // Where the card a dragged tab is drawn as sits in the tab.
    QPoint m_tab_drag_card_offset;
    QObject* m_strip_drag_cancellation_filter { nullptr };
    QTimer* m_tab_preview_timer { nullptr };
    QTimer* m_tab_preview_pointer_timer { nullptr };
    TabPreviewPopup* m_tab_preview_popup { nullptr };
    int m_tab_preview_index { -1 };
    QPointer<Tab> m_previewed_tab;
    QMetaObject::Connection m_tab_preview_paint_connection;
};

class TabWidget final : public QWidget {
    Q_OBJECT

public:
    AK_ALLOC_WITH_KMALLOC;

    TabWidget(WebView::IsPrivate, QWidget* parent);

    LadybirdButton& ladybird_button() const { return *m_ladybird_button; }

    TabBar* tab_bar() const { return m_tab_bar; }

    // Whether a tab dragged to a position, in the given widget, lands in the tab strip rather than leaving it.
    bool tab_drop_area_contains(QWidget const&, QPoint position) const;
    QWidget* tab_bar_row() const { return m_tab_bar_row; }

    void add_tab(Tab* widget, QString const& label);
    void insert_tab(int index, Tab* widget, QString const& label);
    void remove_tab(int index);
    Tab* take_tab(int index);

    int count() const { return m_tab_bar->count(); }

    int current_index() const { return m_tab_bar->currentIndex(); }
    void set_current_index(int index) { m_tab_bar->setCurrentIndex(index); }

    Tab* tab(int index) const { return as<Tab>(m_stacked_widget->widget(index)); }
    void set_current_tab(Tab*);

    int index_of(Tab* widget) const;

    void set_tab_text(int index, QString const& text) { m_tab_bar->setTabText(index, text); }
    void set_tab_tooltip(int index, QString const& tip) { m_tab_bar->setTabToolTip(index, tip); }
    void set_tab_icon(int index, QIcon const& icon) { m_tab_bar->setTabIcon(index, icon); }

    void set_tab_bar_visible(bool visible);
    void set_new_tab_action(QAction* action);
    void set_window_controls_visible(bool);
    void update_window_button_icons();
    void set_vertical_tabs_enabled(bool);
    void set_vertical_tabs_expanded(bool);
    void set_vertical_tabs_expand_on_hover(bool);
    // How strongly the vertical tabs' titles show, which they don't for long once the sidebar starts to collapse.
    qreal vertical_tab_title_opacity() const;
    bool vertical_tabs_are_toggling() const { return m_vertical_tabs_toggling; }
    qreal vertical_tabs_toggle_collapsedness() const;
    int vertical_tabs_toggle_open_tab_width() const;
    int vertical_tabs_toggle_collapsed_tab_width() const;
    void set_vertical_tabs_position(WebView::VerticalTabsPosition);
    bool can_expand_vertical_tabs_on_hover() const;
    void update_tab_button_visibility();
    void update_tab_layout();
    void place_horizontal_new_tab_button();
    int horizontal_tab_strip_drag_space() const;
    void show_all_tabs();

signals:
    void current_tab_changed(int index);
    void tab_close_requested(int index);

protected:
    virtual void dragEnterEvent(QDragEnterEvent*) override;
    virtual void dragLeaveEvent(QDragLeaveEvent*) override;
    virtual void dragMoveEvent(QDragMoveEvent*) override;
    virtual void dropEvent(QDropEvent*) override;
    virtual bool event(QEvent* event) override;
    virtual bool eventFilter(QObject*, QEvent*) override;
    virtual void resizeEvent(QResizeEvent*) override;

private:
    TabLayout current_tab_layout() const;
    QWidget* tab_drag_area_widget() const;
    bool vertical_tabs_effectively_expanded() const;
    bool cursor_is_over_vertical_tabs() const;
    bool vertical_tabs_are_on_right() const;
    int vertical_tabs_layout_width() const;
    bool should_show_window_controls_in_tab_toolbar() const;
    bool vertical_tabs_hold_window_controls() const;
    int vertical_tabs_header_spacing() const;
    int minimum_vertical_tabs_expanded_width() const;

    void rebuild_layout();
    void rebuild_layout_for_horizontal_tabs();
    void place_collapsed_sidebar_buttons();
    void float_sidebar_buttons_while_toggling();
    bool slides_page_with_toggling_sidebar() const;
    void slide_page_with_toggling_sidebar();
    QLabel* m_floating_ladybird_button { nullptr };
    QLabel* m_floating_sidebar_toggle { nullptr };
    // Where the toolbar's slot for the buttons ends when the sidebar starts to open, in the window. The slot keeps
    // ending there until the sidebar is open, so the toolbar's other buttons stay in place under the sidebar.
    void rebuild_layout_for_vertical_tabs();
    void rebuild_page_column();
    void place_toolbar_over_page_column();
    void update_vertical_tabs_column_margins();
    void paint_vertical_tabs_column();
    void update_toolbar_placement();
    void update_tab_toolbar_window_controls_visibility();
    int current_vertical_tabs_width() const;
    void apply_vertical_tabs_expanded_width(int width);
    void persist_vertical_tabs_expanded_width();
    void update_vertical_tabs_resize_handle();
    void update_vertical_tabs_content_separator();
    void set_resize_handle_property(char const* property, bool enabled);
    void update_vertical_tabs_hover_layout();
    QRect vertical_tabs_chrome_rect() const;
    int vertical_tabs_tab_width() const;
    void update_vertical_tabs_button_layout();
    void update_tab_chrome_visibility();
    void recreate_icons();
    void update_vertical_tabs_toggle_icon();
    void update_chrome_style();
    void update_vertical_tabs_overlay_geometry();
    void update_vertical_tabs_content_overlay();
    void set_vertical_tabs_hover_expanded(bool);
    void defer_update_vertical_tabs_hover_expanded();
    void update_vertical_tabs_hover_expanded();
    void toggle_window_maximized();
    bool start_window_move();
    void accept_tab_drag(QDragMoveEvent*, QWidget* target);
    void accept_tab_drop(QDropEvent*, int index);

    TabBar* m_tab_bar { nullptr };
    void size_hidden_pages_like_the_current_one();

    QStackedWidget* m_stacked_widget { nullptr };
    QToolButton* m_new_tab_button { nullptr };
    QToolButton* m_all_tabs_button { nullptr };
    AllTabsPopover* m_all_tabs_popover { nullptr };
    Vector<WebView::SessionWindowId> m_recently_closed_unit_ids;
    LadybirdButton* m_ladybird_button { nullptr };
    QToolButton* m_vertical_tabs_toggle_button { nullptr };
    QToolButton* m_minimize_window_button { nullptr };
    QToolButton* m_maximize_window_button { nullptr };
    QToolButton* m_close_window_button { nullptr };
    QWidget* m_window_controls { nullptr };
    QStackedWidget* m_toolbar_container { nullptr };
    QWidget* m_page_column { nullptr };
    QWidget* m_toolbar_placeholder { nullptr };
    bool m_toolbar_follows_page_column { false };
    QWidget* m_tab_bar_row { nullptr };
    QWidget* m_vertical_tabs_reserved_space { nullptr };
    QWidget* m_vertical_tab_bar_column { nullptr };
    QWidget* m_vertical_tabs_content_separator { nullptr };
    QWidget* m_vertical_tabs_resize_handle { nullptr };
    QWidget* m_vertical_tabs_content { nullptr };
    QTimer* m_vertical_tabs_hover_expand_timer { nullptr };
    QTimer* m_vertical_tabs_hover_collapse_timer { nullptr };
    QVariantAnimation* m_vertical_tabs_hover_expand_animation { nullptr };
    QBoxLayout* m_main_layout { nullptr };
    QBoxLayout* m_tab_bar_row_layout { nullptr };
    QBoxLayout* m_page_column_layout { nullptr };
    QBoxLayout* m_vertical_tab_bar_column_layout { nullptr };
    QBoxLayout* m_vertical_tabs_content_layout { nullptr };
    QSpacerItem* m_vertical_tabs_header_spacer { nullptr };
    bool m_tab_bar_visible { true };
    bool m_window_controls_visible { true };
    bool m_vertical_tabs_enabled { false };
    bool m_vertical_tabs_expanded { true };
    bool m_vertical_tabs_expand_on_hover { false };
    WebView::VerticalTabsPosition m_vertical_tabs_position { WebView::VerticalTabsPosition::Left };
    bool m_vertical_tabs_hover_expanded { false };
    qreal m_vertical_tabs_hover_expand_progress { 0.0 };
    // Opening or closing the sidebar animates its width, from the width it had to the one it will have. The page keeps
    // the room it had until the end.
    QVariantAnimation* m_vertical_tabs_toggle_animation { nullptr };
    qreal m_vertical_tabs_toggle_progress { 1.0 };
    int m_vertical_tabs_toggle_from_width { 0 };
    int m_vertical_tabs_toggle_to_width { 0 };
    int m_vertical_tabs_toggle_layout_width { 0 };
    bool m_vertical_tabs_toggling { false };
    bool m_vertical_tabs_collapse_pending { false };
    bool m_is_resizing_vertical_tabs { false };
    bool m_is_updating_chrome_style { false };
    int m_vertical_tabs_expanded_width { 0 };
    int m_vertical_tabs_resize_start_global_x { 0 };
    int m_vertical_tabs_resize_start_width { 0 };
};

class TabBarButton final : public QPushButton {
    Q_OBJECT

public:
    AK_ALLOC_WITH_KMALLOC;

    enum class Role : u8 {
        Close,
        Audio,
    };

    // Where the button sits: on a tab of the strip or a pinned tile, on a row of the expanded sidebar, or as a badge on
    // a favicon in the collapsed sidebar.
    enum class Placement : u8 {
        Tab,
        SidebarRow,
        RailBadge,
    };

    TabBarButton(QIcon const& icon, Role, QWidget* parent = nullptr);
    void set_placement(Placement);
    // Shows the button by fading it in, or fades it out and hides it.
    void set_revealed(bool);

protected:
    virtual bool event(QEvent* event) override;
    virtual void paintEvent(QPaintEvent*) override;

private:
    void update_size();

    Role m_role { Role::Close };
    Placement m_placement { Placement::Tab };
    bool m_revealed { false };
    qreal m_opacity { 1 };
    QVariantAnimation* m_reveal_animation { nullptr };
};

}
