/*
 * Copyright (c) 2024-2026, Tim Flynn <trflynn89@ladybird.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Function.h>
#include <AK/ScopeGuard.h>
#include <AK/StdLibExtras.h>
#include <AK/kmalloc.h>
#include <LibCore/EventLoop.h>
#include <LibWebView/HistoryStore.h>
#include <LibWebView/SessionStore.h>
#include <UI/Qt/AllTabsPopover.h>
#include <UI/Qt/Application.h>
#include <UI/Qt/BrowserWindow.h>
#include <UI/Qt/ChromeLayout.h>
#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/Icon.h>
#include <UI/Qt/LadybirdButton.h>
#if defined(AK_OS_MACOS)
#    include <UI/Qt/MacWindow.h>
#endif
#include <UI/Qt/HiddenPageSizing.h>
#include <UI/Qt/Menu.h>
#include <UI/Qt/Motion.h>
#include <UI/Qt/Popover.h>
#include <UI/Qt/StringUtils.h>
#include <UI/Qt/SystemAppearance.h>
#include <UI/Qt/TabBar.h>
#include <UI/Qt/Theme.h>
#if defined(LADYBIRD_HAS_WAYLAND_CLIENT)
#    include <UI/Qt/WaylandWindow.h>
#endif
#include <UI/Qt/WindowControlButton.h>

#include <QApplication>
#include <QContextMenuEvent>
#include <QCursor>
#include <QDataStream>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEasingCurve>
#include <QEvent>
#include <QFontMetrics>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QGraphicsPixmapItem>
#include <QGraphicsScene>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLayoutItem>
#include <QLinearGradient>
#include <QMimeData>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QScreen>
#include <QStaticText>
#include <QStyle>
#include <QStyleOptionButton>
#include <QStyleOptionToolButton>
#include <QStylePainter>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>
#include <QVariant>
#include <QVariantAnimation>
#include <QWheelEvent>
#include <QWindow>
#include <QtMath>

namespace Ladybird {

static constexpr auto LADYBIRD_TAB_MIME_TYPE = "application/x-ladybird-tab";
static constexpr int HORIZONTAL_TAB_STRIP_HEIGHT = 35;
// Horizontal tabs reach the bottom of the strip, where the selected tab joins the toolbar below it. What belongs to the
// tabs, such as their contents and the new tab button, is centered on the middle of the selected tab, 20 px down the
// strip. What belongs to the window, such as the window controls and the Ladybird button, is centered on the traffic
// lights' line, 18 px down the strip.
static constexpr int HORIZONTAL_TAB_HEIGHT = 34;
// NB: This is where the 12 px traffic lights are centered, at their inset from the window's top.
static constexpr int HORIZONTAL_WINDOW_ITEM_CENTER = 18;
// NB: The tab row starts below the top of the strip, so a window control as tall as this, at the top of the row, is
//     centered on the window items' line.
static constexpr int HORIZONTAL_WINDOW_CONTROL_HEIGHT = 2 * (HORIZONTAL_WINDOW_ITEM_CENTER - (HORIZONTAL_TAB_STRIP_HEIGHT - HORIZONTAL_TAB_HEIGHT));
// The first item on the strip is as far from the window's edge as the traffic lights are from its corner on macOS.
static constexpr int HORIZONTAL_TAB_STRIP_LEADING_MARGIN = use_native_macos_window_controls() ? NATIVE_MACOS_WINDOW_CONTROLS_INSET : 13;
// Horizontal tabs shrink to this width, and past that the strip scrolls.
static constexpr int HORIZONTAL_TAB_MIN_WIDTH = 112;
// After the pointer leaves the tab bar, the tabs keep their locked widths for this long.
static constexpr int TAB_WIDTH_UNLOCK_DELAY_MS = 300;
static constexpr qreal ATTENTION_DOT_RADIUS = 3.5;
// While the strip scrolls, its ends fade into the strip over this width.
static constexpr int HORIZONTAL_TAB_STRIP_FADE_WIDTH = 28;
// Below this content width, a horizontal tab shows its favicon alone.
static constexpr int HORIZONTAL_TAB_MIN_CONTENT_WIDTH_FOR_TITLE = 48;
static constexpr int HORIZONTAL_TAB_MAX_WIDTH = 232;
// A pinned tab's tile shows only its favicon. In the sidebar, the tiles sit in a grid of this many columns.
static constexpr int PINNED_TAB_COLUMNS = 4;
static constexpr int HORIZONTAL_PINNED_TAB_WIDTH = 44;
// The line between the sidebar's pinned tiles and its other tabs, with the room around it.
static constexpr int PINNED_TABS_SEPARATOR_HEIGHT = 15;
// Rows in the sidebar are 30 px, a point apart.
static constexpr int VERTICAL_TAB_HEIGHT = 31;
static constexpr qreal VERTICAL_TAB_RADIUS = 6;
static constexpr qreal COLLAPSED_VERTICAL_TAB_RADIUS = 7;
static constexpr int VERTICAL_TABS_COLLAPSED_WIDTH = browser_chrome_layout_policy().collapsed_sidebar_width;
static constexpr int VERTICAL_TABS_DEFAULT_EXPANDED_WIDTH = browser_chrome_layout_policy().expanded_sidebar_width;
static constexpr int VERTICAL_TABS_MIN_EXPANDED_WIDTH = 190;
static constexpr int VERTICAL_TABS_MAX_EXPANDED_WIDTH = 400;
static constexpr int VERTICAL_TABS_RESIZE_HIT_AREA_WIDTH = 5;
static constexpr int VERTICAL_TABS_COLLAPSED_SIDE_MARGIN = 6;
static constexpr int VERTICAL_TABS_EXPANDED_SIDE_MARGIN = 3;
// A collapsed sidebar's tab is a card as far from the sidebar's edge as an open sidebar's tab, and its favicon sits where
// the open sidebar's tab has it, so the tabs only grow as the sidebar opens.
static constexpr int COLLAPSED_VERTICAL_TAB_CARD_INSET = 2;
static constexpr int VERTICAL_TAB_CONTENT_LEADING_INSET = 10;
static constexpr int VERTICAL_TABS_TOP_MARGIN = 8;
static constexpr int VERTICAL_TABS_TOGGLE_BUTTON_SIZE = 32;
// Beside the collapsed sidebar, the toggle sits in the toolbar and is as large as the toolbar's buttons.
static constexpr QSize COLLAPSED_VERTICAL_TABS_TOGGLE_BUTTON_SIZE { 32, 28 };
// The collapsed sidebar's corner below the toolbar, beside the page, is as round as the page's own corners.
static constexpr int TAB_CARD_SHAPE_HORIZONTAL_INSET = 5;
// A vertical tab's card fills its row, but for the point between it and the next.
static constexpr int TAB_CARD_SHAPE_TOP_INSET = 0;
static constexpr int TAB_CARD_SHAPE_BOTTOM_INSET = 1;
// The sidebar's tab bar leaves this much room above its first tab and below its last.
static constexpr int VERTICAL_TAB_BAR_END_ROOM = 2;
// A horizontal tab's card holds its contents. It is inset from the tab on every side.
static constexpr qreal HORIZONTAL_TAB_CARD_SHAPE_HORIZONTAL_INSET = 3;
// NB: The card is as low in the tab as the selected tab's body is, so the contents of both share the strip's line.
static constexpr int HORIZONTAL_TAB_CARD_SHAPE_TOP_INSET = 5;
static constexpr int HORIZONTAL_TAB_CARD_SHAPE_BOTTOM_INSET = 1;
// The selected tab fills its whole tab, with rounded top corners, and joins the toolbar with an inverted curve at the
// foot of each side.
static constexpr qreal HORIZONTAL_SELECTED_TAB_RADIUS = 8;
// The selected tab's top sits this far below the top of the other tabs, 5 px down the strip, while its contents stay
// centered with theirs.
static constexpr int HORIZONTAL_SELECTED_TAB_TOP_INSET = 4;
static constexpr qreal HORIZONTAL_TAB_FOOT_RADIUS = 8;
// A dragged tab is drawn as a card with a shadow. Its image leaves room for the shadow.
static constexpr int TAB_DRAG_IMAGE_MARGIN = 48;
// A tab starts to be dragged once the mouse has moved this far with the button down.
static constexpr int TAB_DRAG_START_DISTANCE = 4;
// How far off the tab strip a dragged tab can be pulled, or dropped, and still stay in it.
static constexpr int TAB_DETACH_DISTANCE = 40;
// A closing tab fades out over this much of the time the other tabs take to close the gap it leaves.
static constexpr qreal CLOSING_TAB_FADE_PORTION = 0.4;
// The first and last tabs are this much wider, on the outer side, to make room for their feet at the ends of the tab bar.
static constexpr int HORIZONTAL_TAB_END_ROOM = 8;
static_assert(HORIZONTAL_TAB_END_ROOM >= HORIZONTAL_TAB_FOOT_RADIUS);
// Each end of a tab's content is about as far from the tab's side as it is from the tab's top: the favicon 7 px in from
// the left, and the close button's glyph 11 px in from the right.
static constexpr int HORIZONTAL_TAB_CONTENT_LEFT_PADDING = 4;
static constexpr int HORIZONTAL_TAB_CONTENT_RIGHT_PADDING = 2;
static constexpr int HORIZONTAL_TAB_CONTENT_GAP = 8;
// The buttons after the tabs each take a slot of this size. The new tab button is a little smaller, in the middle of its
// slot, and draws its icon smaller.
static constexpr int HORIZONTAL_NEW_TAB_BUTTON_SIZE = 28;
static constexpr int HORIZONTAL_NEW_TAB_BUTTON_FILL_SIZE = 26;
// NB: The new tab button takes clicks from around its fill as well.
static constexpr int HORIZONTAL_NEW_TAB_BUTTON_HIT_SIZE = 32;
static constexpr int HORIZONTAL_NEW_TAB_BUTTON_ICON_SIZE = 14;
static constexpr qreal HORIZONTAL_NEW_TAB_BUTTON_RADIUS = 6;
// The strip always keeps this much free after the new tab button, before the window controls or the window's edge, so
// there is somewhere to grab the window by however many tabs are open.
static constexpr int HORIZONTAL_TAB_STRIP_DRAG_SPACE = 44;
// The new tab button's slot starts this far after the last tab, not counting the last tab's room for its foot, which
// puts its fill 8 px after the tab, just clear of a selected tab's foot.
static constexpr int HORIZONTAL_NEW_TAB_BUTTON_GAP = 7;
// The Ladybird button keeps 24 px clear to the first tab's straight side after it. That tab's foot reaches into the gap.
static constexpr int LADYBIRD_BUTTON_TRAILING_GAP = 24;
// In the open sidebar, the Ladybird button sits just before the sidebar's toggle, and the toggle a little further from
// the page than the tabs below it.
// NB: The gap matches the toolbar's spacing, so the two keep their distance as they move between the sidebar's header
//     and the toolbar.
static constexpr int SIDEBAR_LADYBIRD_BUTTON_GAP = 2;
static constexpr int SIDEBAR_TOGGLE_PAGE_SIDE_MARGIN = 5;
static constexpr qreal HORIZONTAL_TAB_SEPARATOR_WIDTH = 1;
static constexpr qreal HORIZONTAL_TAB_SEPARATOR_HEIGHT = 16;
static constexpr int TAB_CONTENT_HORIZONTAL_INSET = 8;
static constexpr int VERTICAL_TABS_HOVER_COLLAPSE_POLL_INTERVAL_MS = 250;
static constexpr int TAB_PREVIEW_HOVER_DELAY_MS = 350;
static constexpr int TAB_PREVIEW_THUMBNAIL_WIDTH = 320;
static constexpr int TAB_PREVIEW_THUMBNAIL_HEIGHT = 180;
// NB: The shadow falls downward, so it needs less room above the card.
static constexpr QMargins TAB_PREVIEW_SHADOW_MARGINS { 60, 36, 60, 90 };
static constexpr int TAB_PREVIEW_CARD_GAP = 6;
// A tab's close button is a smaller square than its speaker button, and both draw their icons at the same size. The
// close button takes clicks from the whole height of its tab, in a slot a little wider than its square.
static constexpr int TAB_CLOSE_BUTTON_SIZE = 18;
static constexpr QSize TAB_CLOSE_BUTTON_HIT_SIZE { 20, 28 };
static constexpr int TAB_AUDIO_BUTTON_SIZE = 20;
static constexpr int TAB_BUTTON_ICON_SIZE = 14;
static constexpr qreal TAB_BUTTON_RADIUS = 4;
static constexpr int DROP_PREVIEW_DURATION_MS = 150;
static constexpr int TAB_TITLE_FADE_WIDTH = 24;
// In the sidebar's rows, the speaker is a larger button with a larger icon, so that it matches the close button's weight.
static constexpr int SIDEBAR_ROW_AUDIO_BUTTON_SIZE = 22;
static constexpr int SIDEBAR_ROW_AUDIO_ICON_SIZE = 16;
static constexpr int SIDEBAR_ROW_BUTTON_GAP = 4;
// In the collapsed sidebar, the close button and the speaker are round badges on opposite corners of the favicon,
// reaching a little past them.
static constexpr int RAIL_BADGE_SIZE = 18;
static constexpr int RAIL_BADGE_CLOSE_ICON_SIZE = 12;
static constexpr int RAIL_BADGE_AUDIO_ICON_SIZE = 13;
static constexpr QPoint RAIL_BADGE_CORNER_OUTSET { 2, 1 };
// While a tab loads, its favicon shrinks to this size inside the loading ring.
static constexpr int LOADING_FAVICON_SIZE = 10;
static constexpr auto VERTICAL_TABS_EXPANDED_PROPERTY = "verticalTabsExpanded";
static constexpr auto VERTICAL_TABS_BUTTON_PROPERTY = "verticalTabsButton";
static constexpr auto VERTICAL_TABS_POSITION_PROPERTY = "verticalTabsPosition";
static constexpr auto VERTICAL_TABS_RESIZE_HANDLE_HOVERED_PROPERTY = "hovered";
static constexpr auto VERTICAL_TABS_RESIZE_HANDLE_ACTIVE_PROPERTY = "active";

static void set_dynamic_property_if_needed(QWidget& widget, char const* property, QVariant const& value)
{
    if (widget.property(property) == value)
        return;

    widget.setProperty(property, value);
    widget.style()->unpolish(&widget);
    widget.style()->polish(&widget);
    widget.update();
}

static QPointer<TabWidget> s_active_tab_drag_source;
static QPointer<Tab> s_active_tab_dragged_tab;
static QPointer<TabWidget> s_pending_tab_drop_target;
static int s_pending_tab_drop_index { -1 };
// A tab dragged in a window of its own stays in that window's strip, rather than leaving a gap there.
static bool s_dragged_tab_has_own_window { false };

static bool compositor_moves_dragged_windows()
{
#if defined(LADYBIRD_HAS_WAYLAND_CLIENT)
    return wayland_compositor_moves_dragged_windows();
#else
    return false;
#endif
}

static void show_window_context_menu(QWidget& widget, QPoint global_position)
{
    if (auto* top_level_window = as_if<BrowserWindow>(widget.window()))
        top_level_window->show_window_context_menu(global_position);
}

static bool window_has_chrome_in_titlebar(QWidget& widget)
{
    auto* top_level_window = qobject_cast<BrowserWindow*>(widget.window());
    return !top_level_window || top_level_window->has_chrome_in_titlebar();
}

static bool target_can_accept_tab_drop(QWidget& target, QDropEvent& event)
{
    if (!s_active_tab_drag_source)
        return false;
    if (!event.mimeData()->hasFormat(LADYBIRD_TAB_MIME_TYPE))
        return false;

    auto* source_window = qobject_cast<BrowserWindow*>(s_active_tab_drag_source->window());
    auto* target_window = qobject_cast<BrowserWindow*>(target.window());
    if (!source_window || !target_window)
        return false;

    return source_window->is_private() == target_window->is_private();
}

static void clear_layout(QLayout& layout)
{
    while (auto* item = layout.takeAt(0))
        delete item;
}

static constexpr int vertical_tabs_side_margin(bool expanded)
{
    return expanded ? VERTICAL_TABS_EXPANDED_SIDE_MARGIN : VERTICAL_TABS_COLLAPSED_SIDE_MARGIN;
}

static constexpr int vertical_tabs_horizontal_margin_width(TabLayout tab_layout)
{
    return vertical_tabs_side_margin(tab_layout != TabLayout::VerticalCollapsed) * 2;
}

static constexpr int vertical_tab_width(int available_width, TabLayout tab_layout)
{
    if (available_width > 0)
        return available_width;
    auto width = tab_layout == TabLayout::VerticalCollapsed ? VERTICAL_TABS_COLLAPSED_WIDTH : VERTICAL_TABS_DEFAULT_EXPANDED_WIDTH;
    return width - vertical_tabs_horizontal_margin_width(tab_layout);
}

static char const* vertical_tabs_position_property_value(WebView::VerticalTabsPosition position)
{
    switch (position) {
    case WebView::VerticalTabsPosition::Left:
        return "left";
    case WebView::VerticalTabsPosition::Right:
        return "right";
    }
    VERIFY_NOT_REACHED();
}

static QRectF tab_card_shape_rect(QRectF const& rect)
{
    return rect.adjusted(TAB_CARD_SHAPE_HORIZONTAL_INSET, TAB_CARD_SHAPE_TOP_INSET, -TAB_CARD_SHAPE_HORIZONTAL_INSET, -TAB_CARD_SHAPE_BOTTOM_INSET);
}

static HorizontalTabEndRoom settled_horizontal_tab_end_room(int index, int count)
{
    return { index == 0 ? static_cast<qreal>(HORIZONTAL_TAB_END_ROOM) : 0, index == count - 1 ? static_cast<qreal>(HORIZONTAL_TAB_END_ROOM) : 0 };
}

static QRectF horizontal_tab_card_shape_rect(QRectF const& rect, HorizontalTabEndRoom end_room = {})
{
    auto left_inset = HORIZONTAL_TAB_CARD_SHAPE_HORIZONTAL_INSET + end_room.left;
    auto right_inset = HORIZONTAL_TAB_CARD_SHAPE_HORIZONTAL_INSET + end_room.right;
    return rect.adjusted(left_inset, HORIZONTAL_TAB_CARD_SHAPE_TOP_INSET, -right_inset, -HORIZONTAL_TAB_CARD_SHAPE_BOTTOM_INSET);
}

// The selected horizontal tab's outline, from the bottom of its left foot, over the top of its body, to the bottom of its
// right foot. Filling it closes it along the bottom edge, where the tab meets the toolbar.
static QPainterPath horizontal_selected_tab_path(QRectF const& body)
{
    auto top_radius = HORIZONTAL_SELECTED_TAB_RADIUS;
    auto flare = HORIZONTAL_TAB_FOOT_RADIUS;
    auto left = body.left();
    auto right = body.right();

    QPainterPath path;
    path.moveTo(left - flare, body.bottom());
    path.arcTo(QRectF(left - (2 * flare), body.bottom() - (2 * flare), 2 * flare, 2 * flare), 270, 90);
    path.lineTo(left, body.top() + top_radius);
    path.arcTo(QRectF(left, body.top(), 2 * top_radius, 2 * top_radius), 180, -90);
    path.lineTo(right - top_radius, body.top());
    path.arcTo(QRectF(right - (2 * top_radius), body.top(), 2 * top_radius, 2 * top_radius), 90, -90);
    path.lineTo(right, body.bottom() - flare);
    path.arcTo(QRectF(right, body.bottom() - (2 * flare), 2 * flare, 2 * flare), 180, 90);
    return path;
}

// The hovered horizontal tab's outline: the selected tab's body, with its rounded top corners but without its feet.
static QPainterPath horizontal_hovered_tab_path(QRectF const& body)
{
    auto top_radius = HORIZONTAL_SELECTED_TAB_RADIUS;
    auto left = body.left();
    auto right = body.right();

    QPainterPath path;
    path.moveTo(left, body.bottom());
    path.lineTo(left, body.top() + top_radius);
    path.arcTo(QRectF(left, body.top(), 2 * top_radius, 2 * top_radius), 180, -90);
    path.lineTo(right - top_radius, body.top());
    path.arcTo(QRectF(right - (2 * top_radius), body.top(), 2 * top_radius, 2 * top_radius), 90, -90);
    path.lineTo(right, body.bottom());
    path.closeSubpath();
    return path;
}

// The selected horizontal tab's body: the whole width of the tab, from just below its top down to the toolbar.
static QRectF horizontal_selected_tab_body_rect(QRectF const& tab_rect, HorizontalTabEndRoom end_room)
{
    auto card = horizontal_tab_card_shape_rect(tab_rect, end_room);
    return { card.left() - HORIZONTAL_TAB_CARD_SHAPE_HORIZONTAL_INSET, tab_rect.top() + HORIZONTAL_SELECTED_TAB_TOP_INSET, card.width() + (2 * HORIZONTAL_TAB_CARD_SHAPE_HORIZONTAL_INSET), tab_rect.height() - HORIZONTAL_SELECTED_TAB_TOP_INSET };
}

static QRect tab_card_shape_rect(QRect const& rect)
{
    return rect.adjusted(TAB_CARD_SHAPE_HORIZONTAL_INSET, TAB_CARD_SHAPE_TOP_INSET, -TAB_CARD_SHAPE_HORIZONTAL_INSET, -TAB_CARD_SHAPE_BOTTOM_INSET);
}

static QColor tab_hover_surface(QPalette const& palette, qreal hover_progress)
{
    auto color = Theme::chrome(palette).hover;
    color.setAlphaF(color.alphaF() * static_cast<float>(hover_progress));
    return color;
}

static QColor horizontal_tab_separator(QPalette const& palette)
{
    return Theme::chrome(palette).separator;
}

// In the sidebar, the selected tab is raised: a card in the toolbar's color, with a fine outline and a soft shadow.
static void paint_selected_vertical_tab(QPainter& painter, QPalette const& palette, QRectF const& shape_rect, qreal radius)
{
    // NB: On a dark sidebar a card of the toolbar's color barely stands out, so there the selected tab is lit instead,
    //     and ringed with a hairline inside its edge.
    if (Theme::is_dark(palette)) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(255, 255, 255, 38));
        painter.drawRoundedRect(shape_rect, radius, radius);
        painter.setPen(QPen(QColor(255, 255, 255, 26), 1));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(shape_rect.adjusted(0.5, 0.5, -0.5, -0.5), radius - 0.5, radius - 0.5);
        return;
    }

    auto shadow = QColor(0, 0, 0, 20);
    auto outline = QColor(0, 0, 0, 15);

    painter.setPen(Qt::NoPen);
    // NB: The shadow falls 1 px down and blurs 2 px, drawn as two layers.
    auto shadow_rect = shape_rect.translated(0, 1);
    painter.setBrush(QColor(shadow.red(), shadow.green(), shadow.blue(), shadow.alpha() / 2));
    painter.drawRoundedRect(shadow_rect.adjusted(-1, -1, 1, 1), radius + 1, radius + 1);
    painter.drawRoundedRect(shadow_rect, radius, radius);
    painter.setBrush(outline);
    painter.drawRoundedRect(shape_rect.adjusted(-1, -1, 1, 1), radius + 1, radius + 1);
    painter.setBrush(Theme::chrome(palette).toolbar);
    painter.drawRoundedRect(shape_rect, radius, radius);
}

static QRectF collapsed_vertical_tab_shape_rect(QRectF const& rect)
{
    return rect.adjusted(COLLAPSED_VERTICAL_TAB_CARD_INSET, TAB_CARD_SHAPE_TOP_INSET, -COLLAPSED_VERTICAL_TAB_CARD_INSET, -TAB_CARD_SHAPE_BOTTOM_INSET);
}

static QRect collapsed_vertical_tab_shape_rect(QRect const& rect)
{
    return rect.adjusted(COLLAPSED_VERTICAL_TAB_CARD_INSET, TAB_CARD_SHAPE_TOP_INSET, -COLLAPSED_VERTICAL_TAB_CARD_INSET, -TAB_CARD_SHAPE_BOTTOM_INSET);
}

class NewTabButton final : public QToolButton {
public:
    AK_ALLOC_WITH_KMALLOC;

    explicit NewTabButton(TabBar& tab_bar, QWidget* parent)
        : QToolButton(parent)
        , m_tab_bar(tab_bar)
    {
    }

private:
    virtual void enterEvent(QEnterEvent* event) override
    {
        QToolButton::enterEvent(event);
        update();
    }

    virtual void leaveEvent(QEvent* event) override
    {
        QToolButton::leaveEvent(event);
        update();
    }

    // NB: On the strip, the new tab button's fill is smaller than the button, which takes clicks from around it too.
    QRectF horizontal_button_fill_rect() const
    {
        if (objectName() != "LadybirdNewTabButton")
            return QRectF(rect());
        return { (width() - HORIZONTAL_NEW_TAB_BUTTON_FILL_SIZE) / 2.0, (height() - HORIZONTAL_NEW_TAB_BUTTON_FILL_SIZE) / 2.0, HORIZONTAL_NEW_TAB_BUTTON_FILL_SIZE, HORIZONTAL_NEW_TAB_BUTTON_FILL_SIZE };
    }

    virtual void paintEvent(QPaintEvent* event) override
    {
        if (!property(VERTICAL_TABS_BUTTON_PROPERTY).toBool()) {
            QToolButton::paintEvent(event);
            return;
        }

        auto expanded = property(VERTICAL_TABS_EXPANDED_PROPERTY).toBool();

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        auto is_horizontal_new_tab_button = m_tab_bar.tab_layout() == TabLayout::Horizontal;
        auto shape_rect = expanded
            ? tab_card_shape_rect(QRectF(rect()))
            : (is_horizontal_new_tab_button
                      ? horizontal_button_fill_rect()
                      : collapsed_vertical_tab_shape_rect(QRectF(rect())));
        // NB: The horizontal new tab button is a rounded square like the toolbar's buttons, while the sidebar's is rounded
        //     like the tabs above it.
        auto radius = is_horizontal_new_tab_button ? HORIZONTAL_NEW_TAB_BUTTON_RADIUS : (expanded ? VERTICAL_TAB_RADIUS : COLLAPSED_VERTICAL_TAB_RADIUS);
        QPainterPath tab_path;
        tab_path.addRoundedRect(shape_rect, radius, radius);
        // NB: Hovered and pressed, the button looks like the toolbar's buttons, though in the shape of a tab.
        auto hovered = underMouse() && rect().contains(mapFromGlobal(QCursor::pos()));
        if (auto colors = ChromeStyle::control_frame_colors(*this); colors.has_value()) {
            // NB: Hovered but not pressed, it lights up like the tabs beside it.
            if (hovered && !isDown())
                colors->background = tab_hover_surface(Theme::chrome_palette(*this), 1.0);
            painter.setBrush(colors->background);
            painter.setPen(colors->border.alpha() > 0 ? QPen(colors->border, 1) : QPen(Qt::NoPen));
            painter.drawPath(tab_path);
        }

        auto contents_rect = shape_rect.toAlignedRect().adjusted(VERTICAL_TAB_CONTENT_LEADING_INSET, 0, -TAB_CONTENT_HORIZONTAL_INSET, 0);
        // NB: The buttons on the strip draw their icons at their own sizes.
        auto icon_size = is_horizontal_new_tab_button ? iconSize() : QSize(TAB_ICON_SIZE, TAB_ICON_SIZE);
        QRect icon_rect {
            expanded ? contents_rect.left() - ((icon_size.width() - TAB_ICON_SIZE) / 2) : (is_horizontal_new_tab_button ? (width() - icon_size.width()) / 2 : rect().center().x() - (icon_size.width() / 2)),
            contents_rect.top() + ((contents_rect.height() - icon_size.height()) / 2),
            icon_size.width(),
            icon_size.height(),
        };
        if (!is_horizontal_new_tab_button && !expanded)
            painter.translate(1.0, 0);
        icon().paint(&painter, icon_rect);
    }

    TabBar& m_tab_bar;
};

class VerticalTabsToggleButton final : public QToolButton {
public:
    AK_ALLOC_WITH_KMALLOC;

    using QToolButton::QToolButton;

private:
    virtual void paintEvent(QPaintEvent*) override
    {
        QStyleOptionToolButton option;
        initStyleOption(&option);

        QStylePainter painter(this);
        ChromeStyle::paint_control_frame(painter, *this);
        painter.drawComplexControl(QStyle::CC_ToolButton, option);
    }
};

class TabPreviewThumbnail final : public QWidget {
public:
    AK_ALLOC_WITH_KMALLOC;

    explicit TabPreviewThumbnail(QWidget* parent)
        : QWidget(parent)
    {
        setFixedSize(TAB_PREVIEW_THUMBNAIL_WIDTH, TAB_PREVIEW_THUMBNAIL_HEIGHT);
    }

    void set_colors(QColor background, QColor border)
    {
        m_background = background;
        m_border = border;
        update();
    }

    void set_pixmap(QPixmap pixmap)
    {
        m_pixmap = AK::move(pixmap);
        update();
    }

private:
    virtual void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        auto outer_rect = rect();
        outer_rect.adjust(0, 0, -1, -1);

        QPainterPath outer_path;
        outer_path.addRoundedRect(outer_rect, 7, 7);
        painter.fillPath(outer_path, m_background);

        if (!m_pixmap.isNull()) {
            auto image_rect = rect().adjusted(1, 1, -1, -1);
            QPainterPath image_path;
            image_path.addRoundedRect(image_rect, 6, 6);

            painter.save();
            painter.setClipPath(image_path);

            auto x = image_rect.x() + (image_rect.width() - m_pixmap.width()) / 2;
            auto y = image_rect.y() + (image_rect.height() - m_pixmap.height()) / 2;
            painter.drawPixmap(x, y, m_pixmap);
            painter.restore();
        }

        painter.setPen(m_border);
        painter.drawPath(outer_path);
    }

    QColor m_background;
    QColor m_border;
    QPixmap m_pixmap;
};

class TabPreviewPopup final : public QWidget {
public:
    AK_ALLOC_WITH_KMALLOC;

    explicit TabPreviewPopup(QWidget* parent)
        // NB: The preview only shows, and the room around it for its shadow lies over the tabs, so it lets the mouse through.
        : QWidget(parent, Qt::ToolTip | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint | Qt::WindowTransparentForInput)
    {
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_TransparentForMouseEvents);

        auto* outer_layout = new QVBoxLayout(this);
        outer_layout->setContentsMargins(TAB_PREVIEW_SHADOW_MARGINS);
        outer_layout->setSpacing(0);

        m_card = new QFrame(this);
        m_card->setObjectName("LadybirdTabPreviewCard");

        cast_popover_shadow(*m_card);

        auto* card_layout = new QVBoxLayout(m_card);
        card_layout->setContentsMargins(10, 10, 10, 10);
        card_layout->setSpacing(8);

        m_thumbnail = new TabPreviewThumbnail(m_card);

        m_title_label = new QLabel(m_card);
        m_title_label->setObjectName("LadybirdTabPreviewTitle");
        m_title_label->setFixedWidth(TAB_PREVIEW_THUMBNAIL_WIDTH);
        m_title_label->setTextFormat(Qt::PlainText);

        m_url_label = new QLabel(m_card);
        m_url_label->setObjectName("LadybirdTabPreviewURL");
        m_url_label->setFixedWidth(TAB_PREVIEW_THUMBNAIL_WIDTH);
        m_url_label->setTextFormat(Qt::PlainText);

        card_layout->addWidget(m_thumbnail);
        card_layout->addWidget(m_title_label);
        card_layout->addWidget(m_url_label);
        outer_layout->addWidget(m_card);
    }

    void set_preview(QPalette const& palette, QString title, QString url, Optional<QPixmap const&> thumbnail)
    {
        update_chrome_style(palette);

        if (title.trimmed().isEmpty())
            title = "Untitled";

        QFontMetrics title_metrics(m_title_label->font());
        QFontMetrics url_metrics(m_url_label->font());

        m_thumbnail->setVisible(thumbnail.has_value());
        if (thumbnail.has_value())
            m_thumbnail->set_pixmap(*thumbnail);
        m_title_label->setText(title_metrics.elidedText(title, Qt::ElideRight, TAB_PREVIEW_THUMBNAIL_WIDTH));
        m_url_label->setText(url_metrics.elidedText(url, Qt::ElideMiddle, TAB_PREVIEW_THUMBNAIL_WIDTH));
        adjustSize();
    }

private:
    void update_chrome_style(QPalette const& palette)
    {
        auto card_background = ChromeStyle::style_sheet_color(ChromeStyle::chrome_surface(palette));
        auto thumbnail_background = ChromeStyle::chrome_surface_recessed(palette);
        auto border_color = ChromeStyle::chrome_control_border(palette);
        auto border = ChromeStyle::style_sheet_color(border_color);
        auto text = ChromeStyle::style_sheet_color(ChromeStyle::chrome_text(palette));
        auto muted_text = ChromeStyle::style_sheet_color(ChromeStyle::chrome_muted_text(palette));

        m_thumbnail->set_colors(thumbnail_background, border_color);

        setStyleSheet(qformatted(R"(
QFrame#LadybirdTabPreviewCard {{
    background: {};
    border: 1px solid {};
    border-radius: 10px;
}}

QLabel#LadybirdTabPreviewTitle {{
    color: {};
    font-weight: 600;
}}

QLabel#LadybirdTabPreviewURL {{
    color: {};
}}
)",
            card_background, border, text, muted_text));
    }

    QFrame* m_card { nullptr };
    TabPreviewThumbnail* m_thumbnail { nullptr };
    QLabel* m_title_label { nullptr };
    QLabel* m_url_label { nullptr };
};

class TabDragCancellationFilter final : public QObject {
public:
    AK_ALLOC_WITH_KMALLOC;

    TabDragCancellationFilter() = default;
    explicit TabDragCancellationFilter(Function<void()> on_cancel)
        : m_on_cancel(move(on_cancel))
    {
    }

    bool was_canceled() const { return m_was_canceled; }

private:
    virtual bool eventFilter(QObject*, QEvent* event) override
    {
        // NB: Escape is also a shortcut, which would take the key before it is pressed in any widget.
        if (event->type() != QEvent::KeyPress && event->type() != QEvent::ShortcutOverride)
            return false;
        if (static_cast<QKeyEvent*>(event)->key() != Qt::Key_Escape)
            return false;
        if (!m_on_cancel) {
            m_was_canceled = true;
            return false;
        }
        // NB: Accepting the shortcut override has Escape pressed as a key, which is where the drag is canceled. That key
        //     press is then taken, so it doesn't also reach the page or the address field.
        event->accept();
        if (event->type() == QEvent::ShortcutOverride)
            return true;
        m_was_canceled = true;
        m_on_cancel();
        return true;
    }

    Function<void()> m_on_cancel;
    bool m_was_canceled { false };
};

// The card a tab dragged within its strip is drawn as, above the rest of the window.
class TabDragCard final : public QWidget {
public:
    AK_ALLOC_WITH_KMALLOC;

    TabDragCard(QWidget& parent, QPixmap pixmap)
        : QWidget(&parent)
        , m_pixmap(AK::move(pixmap))
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        resize(m_pixmap.deviceIndependentSize().toSize());
    }

private:
    virtual void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.drawPixmap(0, 0, m_pixmap);
    }

    QPixmap m_pixmap;
};

TabBar::TabBar(TabWidget* tab_widget)
    : QTabBar(tab_widget)
    , m_tab_widget(tab_widget)
{
    setMouseTracking(true);
    setAcceptDrops(true);
    setFocusPolicy(Qt::NoFocus);
    setIconSize({ 16, 16 });
    setMinimumHeight(HORIZONTAL_TAB_HEIGHT);
    recreate_icons();

    m_tab_width_unlock_timer = new QTimer(this);
    m_tab_width_unlock_timer->setSingleShot(true);
    m_tab_width_unlock_timer->setInterval(TAB_WIDTH_UNLOCK_DELAY_MS);
    QObject::connect(m_tab_width_unlock_timer, &QTimer::timeout, this, [this] {
        m_locked_tab_width = {};
        // NB: QTabBar keeps its tabs' sizes until something changes them, and a font change is what lays them out anew.
        QEvent font_change(QEvent::FontChange);
        changeEvent(&font_change);
        refresh_tab_layout();
    });

    m_tab_preview_timer = new QTimer(this);
    m_tab_preview_timer->setInterval(TAB_PREVIEW_HOVER_DELAY_MS);
    m_tab_preview_timer->setSingleShot(true);
    connect(m_tab_preview_timer, &QTimer::timeout, this, &TabBar::show_tab_preview);

    m_tab_preview_popup = new TabPreviewPopup(this);

    // While the preview shows, the pointer's moves over the tab bar and out of it are followed by where it is.
    m_tab_preview_pointer_timer = new QTimer(this);
    m_tab_preview_pointer_timer->setInterval(50);
    connect(m_tab_preview_pointer_timer, &QTimer::timeout, this, [this] {
        auto position = mapFromGlobal(QCursor::pos());
        if (rect().contains(position))
            set_hovered_tab_index(tab_index_at(position));
        else
            pointer_left();
    });

    m_loading_ring_animation = new QVariantAnimation(this);
    m_loading_ring_animation->setDuration(ChromeStyle::LOADING_RING_PERIOD_MS);
    m_loading_ring_animation->setStartValue(0.0);
    m_loading_ring_animation->setEndValue(1.0);
    m_loading_ring_animation->setLoopCount(-1);
    // NB: Each frame repaints only the loading tabs, and checks again, so the ring also stops turning when the last loading
    //     tab is closed.
    connect(m_loading_ring_animation, &QVariantAnimation::valueChanged, this, [this] {
        auto any_tab_loading = false;
        for (int index = 0; index < count(); ++index) {
            if (loading_ring_opacity(index) <= 0)
                continue;
            any_tab_loading = true;
            update(visual_tab_rect(index));
        }
        if (!any_tab_loading || !isVisible())
            update_loading_ring_animation();
    });

    m_hover_animation = new QVariantAnimation(this);
    m_hover_animation->setDuration(Motion::FAST);
    m_hover_animation->setEasingCurve(Motion::enter_easing());
    connect(m_hover_animation, &QVariantAnimation::valueChanged, this, [this](QVariant const& value) {
        m_hover_progress = value.toReal();
        update();
    });
    connect(m_hover_animation, &QVariantAnimation::finished, this, [this] {
        if (m_hover_progress <= 0.0)
            m_hover_animation_tab_index = -1;
    });

    m_selection_animation = new QVariantAnimation(this);
    m_selection_animation->setDuration(Motion::FAST);
    m_selection_animation->setEasingCurve(Motion::enter_easing());
    m_selection_animation->setStartValue(0.0);
    m_selection_animation->setEndValue(1.0);
    connect(m_selection_animation, &QVariantAnimation::valueChanged, this, [this](QVariant const& value) {
        m_selection_progress = value.toReal();
        update_tab_button_geometry();
        update();
    });
    // NB: The tab the selection moved from is kept until the tabs have also settled, as the lines beside it stay hidden
    //     until then.
    connect(m_selection_animation, &QVariantAnimation::finished, this, [this] {
        if (m_layout_animation_progress >= 1.0)
            m_previously_selected_tab = nullptr;
        update();
    });
    QPointer<Tab> selected_tab;
    connect(this, &QTabBar::currentChanged, this, [this, selected_tab](int index) mutable {
        auto* tab = m_tab_widget ? m_tab_widget->tab(index) : nullptr;
        // NB: A selection that moves because its tab closed fades from the picture of the closing tab, if there is one.
        m_previously_selected_tab = selected_tab;
        selected_tab = tab;
        if ((!m_previously_selected_tab && !m_closing_tab.has_value()) || !isVisible()) {
            m_selection_animation->stop();
            m_selection_progress = 1.0;
            return;
        }
        m_selection_animation->stop();
        // NB: The animation only reports its start value on its first step, after the tab bar may have repainted.
        m_selection_progress = 0.0;
        m_selection_animation->start();
    });

    m_layout_animation = new QVariantAnimation(this);
    m_layout_animation->setDuration(Motion::BASE);
    m_layout_animation->setEasingCurve(Motion::move_easing());
    m_layout_animation->setStartValue(0.0);
    m_layout_animation->setEndValue(1.0);
    connect(m_layout_animation, &QVariantAnimation::valueChanged, this, [this](QVariant const& value) {
        m_layout_animation_progress = value.toReal();
        update_tab_button_geometry();
        if (m_tab_widget) {
            // NB: While tabs close, the tabs after them slide back from beyond where the tabs will end, so the bar
            //     keeps room for them.
            if (horizontal_tabs_end_offset() > 0 || m_layout_animation_progress >= 1.0)
                m_tab_widget->update_tab_layout();
            else
                m_tab_widget->place_horizontal_new_tab_button();
        }
        update();
    });
    connect(m_layout_animation, &QVariantAnimation::finished, this, [this] {
        m_closing_tab.clear();
        if (m_selection_animation->state() != QAbstractAnimation::Running)
            m_previously_selected_tab = nullptr;
        update();
    });

    m_drop_preview_animation = new QVariantAnimation(this);
    m_drop_preview_animation->setDuration(DROP_PREVIEW_DURATION_MS);
    m_drop_preview_animation->setEasingCurve(QEasingCurve::OutCubic);
    m_drop_preview_animation->setStartValue(0.0);
    m_drop_preview_animation->setEndValue(1.0);
    connect(m_drop_preview_animation, &QVariantAnimation::valueChanged, this, [this](QVariant const& value) {
        m_drop_preview_progress = value.toReal();
        if (m_drop_preview_start.strip_growth != m_drop_preview_target.strip_growth)
            updateGeometry();
        update_tab_button_geometry();
        update();
    });

    connect(this, &QTabBar::currentChanged, this, [this](int index) {
        hide_tab_preview();
        ensure_tab_visible(index);
        update_tab_button_geometry();
    });
}

void TabBar::set_available_width(int width)
{
    if (m_available_width != width) {
        m_available_width = width;
        refresh_tab_layout();
    }
}

void TabBar::set_tab_layout(TabLayout tab_layout)
{
    if (m_tab_layout == tab_layout)
        return;

    m_tab_layout = tab_layout;
    m_scroll_offset = 0;
    m_locked_tab_width = {};

    if (m_tab_layout == TabLayout::Horizontal) {
        setShape(QTabBar::RoundedNorth);
        setMinimumSize({ 0, HORIZONTAL_TAB_HEIGHT });
        setMaximumWidth(QWIDGETSIZE_MAX);
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        setUsesScrollButtons(false);
    } else {
        setShape(vertical_tabs_are_on_right() ? QTabBar::RoundedEast : QTabBar::RoundedWest);
        setMinimumSize({ 0, 0 });
        setMaximumWidth(QWIDGETSIZE_MAX);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        setUsesScrollButtons(false);
    }

    refresh_tab_layout();
}

void TabBar::set_pinned_tabs_in_rows(bool in_rows)
{
    if (m_pinned_tabs_in_rows == in_rows)
        return;

    m_pinned_tabs_in_rows = in_rows;
    refresh_tab_layout();
}

void TabBar::set_vertical_tabs_position(WebView::VerticalTabsPosition position)
{
    if (m_vertical_tabs_position == position)
        return;

    m_vertical_tabs_position = position;
    if (m_tab_layout != TabLayout::Horizontal)
        setShape(vertical_tabs_are_on_right() ? QTabBar::RoundedEast : QTabBar::RoundedWest);
    hide_tab_preview();
    refresh_tab_layout();
}

void TabBar::lock_tab_widths()
{
    if (tab_layout() != TabLayout::Horizontal)
        return;

    auto pinned_count = pinned_tab_count();
    if (pinned_count >= count())
        return;

    // NB: Only an unpinned tab between the first and the last has no room for a foot.
    auto end_room = (pinned_count == 0 ? HORIZONTAL_TAB_END_ROOM : 0) + (pinned_count == count() - 1 ? HORIZONTAL_TAB_END_ROOM : 0);
    m_locked_tab_width = tabRect(pinned_count).width() - end_room;
    m_tab_width_unlock_timer->stop();
}

void TabBar::refresh_tab_layout()
{
    // NB: Qt lays the tabs out only as the bar resizes, but their size can change while the bar's does not, such as when
    //     the sidebar collapses to the width its tab bar had in the open layout.
    QResizeEvent resize_event(size(), size());
    QTabBar::resizeEvent(&resize_event);
    set_scroll_offset(m_scroll_offset);
    updateGeometry();
    update_tab_button_geometry();
    update();
}

int TabBar::pinned_tab_count() const
{
    if (!m_tab_widget)
        return 0;

    // While a tab is being taken, its page leaves the stack before its tab-bar entry, so a missing page ends the count.
    int pinned_count = 0;
    while (pinned_count < count()) {
        auto* tab = m_tab_widget->tab(pinned_count);
        if (!tab || !tab->is_pinned())
            break;
        ++pinned_count;
    }
    return pinned_count;
}

void TabBar::recreate_icons()
{
    m_fallback_tab_icon = create_chrome_icon(ChromeIcon::Globe, Theme::chrome_palette(*this));
    update();
}

QSize TabBar::sizeHint() const
{
    if (tab_layout() != TabLayout::Horizontal)
        return vertical_size_hint(count()) + QSize(0, qRound(current_strip_growth()));
    return QTabBar::sizeHint();
}

QSize TabBar::minimumSizeHint() const
{
    // However many tabs are pinned, one row is enough: Pinned tabs that don't fit in half the bar scroll with the rest.
    if (tab_layout() != TabLayout::Horizontal)
        return { vertical_tab_width(m_available_width, tab_layout()), count() > 0 ? (2 * VERTICAL_TAB_BAR_END_ROOM) + VERTICAL_TAB_HEIGHT : 0 };
    return QTabBar::minimumSizeHint();
}

QSize TabBar::tabSizeHint(int index) const
{
    auto hint = QTabBar::tabSizeHint(index);

    if (tab_layout() != TabLayout::Horizontal) {
        hint.setWidth(vertical_tab_width(m_available_width, tab_layout()));
        hint.setHeight(VERTICAL_TAB_HEIGHT);
        return hint;
    }

    if (auto count = this->count(); count > 0) {
        auto pinned_count = pinned_tab_count();
        // The first and last tabs also take room for their feet at the ends of the tab bar.
        auto end_room = (index == 0 ? HORIZONTAL_TAB_END_ROOM : 0) + (index == count - 1 ? HORIZONTAL_TAB_END_ROOM : 0);
        if (index < pinned_count) {
            hint.setWidth(HORIZONTAL_PINNED_TAB_WIDTH + end_room);
            hint.setHeight(HORIZONTAL_TAB_HEIGHT);
            return hint;
        }

        if (m_locked_tab_width.has_value()) {
            hint.setWidth(*m_locked_tab_width + end_room);
            hint.setHeight(HORIZONTAL_TAB_HEIGHT);
            return hint;
        }

        // The unpinned tabs share whatever width the pinned tiles and the room at the ends leave.
        auto available_width = (m_available_width > 0 ? m_available_width : this->width()) - (pinned_count * HORIZONTAL_PINNED_TAB_WIDTH) - (2 * HORIZONTAL_TAB_END_ROOM);
        auto width = available_width / max(1, count - pinned_count);
        width = min(HORIZONTAL_TAB_MAX_WIDTH, width);
        width = max(HORIZONTAL_TAB_MIN_WIDTH, width);

        hint.setWidth(width + end_room);
    }

    hint.setHeight(HORIZONTAL_TAB_HEIGHT);
    return hint;
}

void TabBar::resizeEvent(QResizeEvent* event)
{
    hide_tab_preview();
    QTabBar::resizeEvent(event);
    set_scroll_offset(m_scroll_offset);
    ensure_tab_visible(currentIndex());
    update_tab_button_geometry();
}

void TabBar::tabLayoutChange()
{
    QTabBar::tabLayoutChange();
    update_layout_animation();
    set_scroll_offset(m_scroll_offset);
    update_tab_button_geometry();

    if (m_tab_preview_index < 0)
        return;

    // NB: Title and favicon updates also change the tab layout. Keep the preview open if it still belongs
    //     to the tab under the pointer, but dismiss it if tabs moved or were removed.
    auto hovered_tab = tab_index_at(mapFromGlobal(QCursor::pos()));
    if (hovered_tab != m_tab_preview_index || !m_tab_widget
        || (m_previewed_tab && m_tab_widget->tab(hovered_tab) != m_previewed_tab)) {
        hide_tab_preview();
        return;
    }

    if (m_previewed_tab)
        show_tab_preview();
}

bool TabBar::event(QEvent* event)
{
    // The separators between tabs are mixed from the tab strip's color, which follows the window's activity.
    if (event->type() == QEvent::WindowActivate || event->type() == QEvent::WindowDeactivate)
        update();

    // The ring stops turning while the tab bar is hidden, so it has to start again for tabs still loading when it is shown.
    if (event->type() == QEvent::Show)
        update_loading_ring_animation();

    if (event->type() == QEvent::ToolTip) {
        auto* help_event = static_cast<QHelpEvent*>(event);
        auto hovered_tab = tab_index_at(help_event->pos());
        if (hovered_tab >= 0 && hovered_tab != currentIndex()) {
            if (auto* tab = m_tab_widget ? m_tab_widget->tab(hovered_tab) : nullptr) {
                if (!tab->is_loaded() || tab->view().tab_preview_pixmap({ TAB_PREVIEW_THUMBNAIL_WIDTH, TAB_PREVIEW_THUMBNAIL_HEIGHT }).has_value()) {
                    event->accept();
                    return true;
                }
            }
        }

        // The selected tab shows no preview, so a title too long for it shows in full, with the site, in a tooltip.
        if (hovered_tab >= 0 && hovered_tab == currentIndex()) {
            auto* tab = m_tab_widget ? m_tab_widget->tab(hovered_tab) : nullptr;
            if (tab && m_tabs_with_cut_titles.contains(tab)) {
                auto text = tabText(hovered_tab);
                if (auto host = tab->view().url().serialized_host(); !host.is_empty())
                    text = qformatted("{}\n{}", ak_string_from_qstring(text), host);
                QToolTip::showText(help_event->globalPos(), text, this, tabRect(hovered_tab));
                event->accept();
                return true;
            }
        }
    }

    return QTabBar::event(event);
}

void TabBar::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    auto is_vertical = tab_layout() != TabLayout::Horizontal;
    auto is_collapsed_vertical = tab_layout() == TabLayout::VerticalCollapsed;
    auto dragged_index = dragged_tab_index();
    auto hover_progress_of = [&](int index) {
        if (index == m_tab_painted_unselected)
            return 0.0;
        return index == m_hover_animation_tab_index ? m_hover_progress : (index == m_hovered_tab_index ? 1.0 : 0.0);
    };
    auto region = pinned_region();
    auto pinned_count = is_vertical ? region.count : pinned_tab_count();
    // While tabs make way for a pinned tab dragged in from another window, the pinned tabs can take up another row.
    auto pinned_region_growth = qRound(current_pinned_region_growth());
    auto fixed_height = region.scrolls ? 0 : region.height + pinned_region_growth;
    // NB: Without pinned tabs in the way, the unpinned tabs also use the room above them.
    auto unpinned_clip_top = fixed_height > 0 ? VERTICAL_TAB_BAR_END_ROOM + fixed_height : 0;
    QRect unpinned_clip { 0, unpinned_clip_top, width(), max(0, height() - unpinned_clip_top) };

    // Titles are shaped once and kept for as long as some tab shows them, since resizing the window repaints the tab
    // bar on every step without changing any title.
    auto title_font = ChromeStyle::chrome_title_font(font());
    if (m_tab_title_texts_font != title_font) {
        m_tab_title_texts.clear();
        m_tab_title_texts_font = title_font;
    }
    QHash<QString, QStaticText> title_texts;
    HashTable<Tab const*> tabs_with_cut_titles;
    auto title_line_height = QFontMetricsF(title_font).height();

    if (auto rect = closing_tab_rect(); !rect.isEmpty()) {
        painter.save();
        // NB: The closing tab narrows from its end, which is rounded like the top corners of a tab. Its bottom reaches
        //     past the clip's rounded corners, as the selected tab joins the toolbar there.
        QPainterPath clip;
        clip.addRoundedRect(QRectF(rect).adjusted(0, 0, 0, HORIZONTAL_SELECTED_TAB_RADIUS), HORIZONTAL_SELECTED_TAB_RADIUS, HORIZONTAL_SELECTED_TAB_RADIUS);
        painter.setClipPath(clip);
        // The closing tab fades out well before the gap it leaves closes, so none of it is seen cut short.
        painter.setOpacity(max(0.0, 1.0 - (m_layout_animation_progress / CLOSING_TAB_FADE_PORTION)));
        painter.drawPixmap(rect.topLeft(), m_closing_tab->image);
        painter.restore();
    }

    if (!is_vertical) {
        // NB: The hovered tabs are filled before any tab is painted, so a selected tab's foot is drawn over the hovered
        //     tab beside it.
        auto paint_hover_shape = [&](int index) {
            if (index < 0 || index >= count() || index == dragged_index)
                return;
            auto hover_progress = hover_progress_of(index);
            if (hover_progress <= 0.0 || selection_of(index) >= 1.0)
                return;
            auto tab_rect = shifted_tab_rect(index);
            if (!tab_rect.isValid() || tab_rect.right() < 0 || tab_rect.left() > width())
                return;
            auto body_rect = horizontal_selected_tab_body_rect(QRectF(tab_rect), horizontal_tab_end_room(index));
            if (body_rect.width() < 2 * HORIZONTAL_SELECTED_TAB_RADIUS)
                return;
            painter.setBrush(tab_hover_surface(Theme::chrome_palette(*this), hover_progress));
            painter.setPen(Qt::NoPen);
            painter.drawPath(horizontal_hovered_tab_path(body_rect));
        };
        paint_hover_shape(m_hover_animation_tab_index);
        if (m_hovered_tab_index != m_hover_animation_tab_index)
            paint_hover_shape(m_hovered_tab_index);
    }

    auto previously_selected_index = m_previously_selected_tab && m_tab_widget ? m_tab_widget->index_of(m_previously_selected_tab) : -1;
    for (int index = 0; index < count(); ++index) {
        // The tab being dragged follows the cursor as the drag's image, leaving a gap where it would be dropped.
        if (index == dragged_index)
            continue;

        // Unpinned tabs scroll under the pinned ones, so they never paint over them. The clips that each tab's title sets
        // later intersect with this one, rather than replacing it.
        if (is_vertical) {
            if (index >= pinned_count)
                painter.setClipRect(unpinned_clip);
            else
                painter.setClipping(false);
        } else {
            painter.setClipping(false);
        }

        auto tab_rect = shifted_tab_rect(index);
        if (!tab_rect.isValid())
            continue;
        if (is_vertical && (tab_rect.bottom() < 0 || tab_rect.top() > height()))
            continue;
        if (!is_vertical && (tab_rect.right() < 0 || tab_rect.left() > width()))
            continue;

        bool is_selected = index == currentIndex();
        auto selection = selection_of(index);
        auto hover_progress = hover_progress_of(index);
        bool is_hovered = hover_progress > 0.0;
        // A pinned tab is a tile that shows only its favicon, except where the pinned tabs keep their rows.
        auto is_pinned_tile = index < pinned_count && pinned_tabs_are_tiles();

        QRectF shape_rect;
        if (is_collapsed_vertical)
            shape_rect = collapsed_vertical_tab_shape_rect(QRectF(tab_rect));
        else if (!is_vertical)
            shape_rect = horizontal_tab_card_shape_rect(QRectF(tab_rect), horizontal_tab_end_room(index));
        else
            shape_rect = tab_card_shape_rect(QRectF(tab_rect));
        QRectF horizontal_body_rect;
        if (!is_vertical)
            horizontal_body_rect = horizontal_selected_tab_body_rect(QRectF(tab_rect), horizontal_tab_end_room(index));
        QPainterPath tab_path;
        // NB: A tab too narrow for the selected tab's rounded corners, such as one just opening, has no room for its shape.
        if (!is_vertical && selection > 0.0 && horizontal_body_rect.width() < 2 * HORIZONTAL_SELECTED_TAB_RADIUS)
            selection = 0.0;
        if (!is_vertical && selection > 0.0)
            tab_path = horizontal_selected_tab_path(horizontal_body_rect);
        auto vertical_tab_radius = is_collapsed_vertical ? COLLAPSED_VERTICAL_TAB_RADIUS : VERTICAL_TAB_RADIUS;
        if (is_vertical)
            tab_path.addRoundedRect(shape_rect, vertical_tab_radius, vertical_tab_radius);

        // Horizontal tabs are divided by lines, except around the selected tab and the hovered one, and between pinned
        // tabs. The last tab has none after it.
        if (!is_vertical) {
            auto next_index = index + 1;
            auto next_is_pinned = next_index < pinned_count;
            // NB: The lines beside the tabs the selection moves between stay hidden until the tabs have settled, so none
            //     shows over a tab still being selected or not, nor over one opening or closing.
            auto selection_is_settling = m_selection_animation->state() == QAbstractAnimation::Running || m_layout_animation_progress < 1.0;
            auto beside_selection_change = selection > 0.0 || selection_of(next_index) > 0.0 || index == previously_selected_index || next_index == previously_selected_index;
            auto separator_opacity = selection_is_settling && beside_selection_change ? 0.0 : 1.0 - max(hover_progress, hover_progress_of(next_index));
            if (next_index < count() && index != currentIndex() && next_index != currentIndex() && !next_is_pinned && separator_opacity > 0.0) {
                auto separator_center_y = shape_rect.center().y();
                // NB: The separator sits just inside the edge between two tabs. This keeps it on whole device pixels.
                auto separator_x = shape_rect.right() + HORIZONTAL_TAB_CARD_SHAPE_HORIZONTAL_INSET - 1;
                QRectF separator_rect { separator_x, separator_center_y - (HORIZONTAL_TAB_SEPARATOR_HEIGHT / 2.0), HORIZONTAL_TAB_SEPARATOR_WIDTH, HORIZONTAL_TAB_SEPARATOR_HEIGHT };
                painter.save();
                painter.setOpacity(separator_opacity);
                painter.fillRect(separator_rect, horizontal_tab_separator(Theme::chrome_palette(*this)));
                painter.restore();
            }
        }

        // NB: The hovered horizontal tabs were filled before any tab was painted.
        if (selection < 1.0 && is_hovered && is_vertical) {
            painter.setBrush(tab_hover_surface(Theme::chrome_palette(*this), hover_progress));
            painter.setPen(Qt::NoPen);
            painter.drawPath(tab_path);
        } else if (selection < 1.0 && is_vertical && !is_collapsed_vertical && is_pinned_tile) {
            // The pinned tiles at the top of the sidebar are always filled, so they read as a grid of buttons. The fill
            // fades as they move into the collapsed sidebar's column, where they are not.
            painter.setBrush(tab_hover_surface(Theme::chrome_palette(*this), 1.0 - region.collapsedness));
            painter.setPen(Qt::NoPen);
            painter.drawPath(tab_path);
        }
        if (selection > 0.0) {
            painter.save();
            painter.setOpacity(selection);
            if (is_vertical) {
                paint_selected_vertical_tab(painter, Theme::chrome_palette(*this), shape_rect, vertical_tab_radius);
            } else {
                painter.setPen(Qt::NoPen);
                painter.setBrush(ChromeStyle::chrome_active_tab_surface(Theme::chrome_palette(*this)));
                painter.drawPath(tab_path);
            }
            painter.restore();
        }

        auto ring_opacity = loading_ring_opacity(index);

        auto icon = tabIcon(index);
        auto* tab_for_index = m_tab_widget ? m_tab_widget->tab(index) : nullptr;
        auto needs_attention = tab_for_index && tab_for_index->needs_attention() && !is_selected;
        auto favicon_shrink = tab_for_index ? tab_for_index->loading_favicon_shrink() : 0;
        auto paint_icon = [&](QRect const& icon_rect) {
            if (ring_opacity > 0) {
                painter.save();
                painter.setOpacity(ring_opacity);
                ChromeStyle::paint_loading_ring(painter, Theme::chrome_palette(*this), QRectF(icon_rect).center(), ChromeStyle::LoadingRingSize::Favicon, tab_for_index && tab_for_index->is_waiting_for_response() ? ChromeStyle::LoadingPhase::WaitingForResponse : ChromeStyle::LoadingPhase::Receiving);
                painter.restore();
            }
            // NB: The fallback globe is drawn on a chrome icon's larger canvas, with room around it, so it is painted at
            //     that size to be about as large as a favicon.
            auto overhang = (CHROME_ICON_CANVAS_SIZE - TAB_ICON_SIZE) / 2;
            auto const& shown_icon = icon.isNull() ? m_fallback_tab_icon : icon;
            auto shown_rect = icon.isNull() ? icon_rect.adjusted(-overhang, -overhang, overhang, overhang) : icon_rect;
            if (favicon_shrink > 0) {
                // While the tab loads, the favicon shrinks to fit inside the ring, so a square favicon never touches it.
                auto scale = 1 - ((1 - (static_cast<qreal>(LOADING_FAVICON_SIZE) / TAB_ICON_SIZE)) * favicon_shrink);
                auto pixmap = shown_icon.pixmap(shown_rect.size(), devicePixelRatioF());
                QRectF target { QPointF(), QSizeF(shown_rect.size()) * scale };
                target.moveCenter(QRectF(shown_rect).center());
                painter.save();
                painter.setRenderHint(QPainter::SmoothPixmapTransform);
                painter.drawPixmap(target, pixmap, QRectF(pixmap.rect()));
                painter.restore();
            } else {
                shown_icon.paint(&painter, shown_rect);
            }

            // A tab with news has a dot on its favicon's corner, cut out of the strip around it.
            if (needs_attention) {
                QPointF dot_center { icon_rect.right() + 0.5, icon_rect.bottom() + 0.5 };
                painter.save();
                painter.setRenderHint(QPainter::Antialiasing);
                painter.setPen(Qt::NoPen);
                auto backing = is_vertical ? ChromeStyle::chrome_sidebar_background(Theme::chrome_palette(*this)) : ChromeStyle::chrome_tab_strip_background(Theme::chrome_palette(*this));
                painter.setBrush(is_hovered ? tab_hover_surface(Theme::chrome_palette(*this), 1.0) : backing);
                painter.drawEllipse(dot_center, ATTENTION_DOT_RADIUS + 1.5, ATTENTION_DOT_RADIUS + 1.5);
                painter.setBrush(Theme::accent());
                painter.drawEllipse(dot_center, ATTENTION_DOT_RADIUS, ATTENTION_DOT_RADIUS);
                painter.restore();
            }
        };

        if (is_collapsed_vertical || is_pinned_tile) {
            // The audio button takes the favicon's place on a pinned tile.
            if (is_pinned_tile && tabButton(index, QTabBar::LeftSide))
                continue;

            auto icon_center = shape_rect.center();
            QRect icon_rect {
                qRound(icon_center.x()) - (TAB_ICON_SIZE / 2),
                qRound(icon_center.y()) - (TAB_ICON_SIZE / 2),
                TAB_ICON_SIZE,
                TAB_ICON_SIZE,
            };
            paint_icon(icon_rect);
            continue;
        }

        // A tab that grows, such as one opening, lays out its contents at the width it grows to, and shows as much of
        // them as it has grown into.
        auto layout_rect = tab_rect;
        if (!is_vertical && is_growing_horizontal_tab(index, tab_rect)) {
            layout_rect.setWidth(visual_tab_rect(index).width());
            painter.setClipRect(shape_rect, Qt::IntersectClip);
        }
        auto contents_rect = is_vertical
            ? shape_rect.toAlignedRect().adjusted(VERTICAL_TAB_CONTENT_LEADING_INSET, 0, -TAB_CONTENT_HORIZONTAL_INSET, 0)
            : horizontal_tab_contents_rect(index, layout_rect).toAlignedRect();
        // The speaker sits after the title, with the close button.
        if (auto* left_button = tabButton(index, QTabBar::LeftSide); left_button && left_button->isVisible())
            contents_rect.setRight(min(contents_rect.right(), left_button->geometry().left() - 6));
        // NB: A horizontal tab keeps its close button's slot whether the button shows or not, so that the title ends at
        //     the same place either way.
        if (auto* right_button = tabButton(index, QTabBar::RightSide)) {
            auto keeps_slot = !is_vertical && right_button->x() - HORIZONTAL_TAB_CONTENT_GAP >= contents_rect.left() + TAB_ICON_SIZE;
            if (right_button->isVisible() || keeps_slot)
                contents_rect.setRight(min(contents_rect.right(), right_button->geometry().left() - 6));
        }

        if (!is_vertical && contents_rect.width() < HORIZONTAL_TAB_MIN_CONTENT_WIDTH_FOR_TITLE) {
            // NB: A tab too narrow for its favicon next to its mute button shows the mute button alone, rather than
            //     drawing the favicon over it.
            if (contents_rect.width() < TAB_ICON_SIZE)
                continue;
            QRect icon_rect {
                contents_rect.left() + ((contents_rect.width() - TAB_ICON_SIZE) / 2),
                contents_rect.top() + ((contents_rect.height() - TAB_ICON_SIZE) / 2),
                TAB_ICON_SIZE,
                TAB_ICON_SIZE,
            };
            paint_icon(icon_rect);
            continue;
        }

        QRect icon_rect {
            contents_rect.left(),
            contents_rect.top() + ((contents_rect.height() - TAB_ICON_SIZE) / 2),
            TAB_ICON_SIZE,
            TAB_ICON_SIZE,
        };
        paint_icon(icon_rect);
        contents_rect.setLeft(icon_rect.right() + HORIZONTAL_TAB_CONTENT_GAP);

        auto title = tabText(index);
        // In an inactive window, the tabs' titles drop to the muted color.
        auto is_inactive = ChromeStyle::window_activity(*this) == ChromeStyle::WindowActivity::Inactive;
        auto unselected_text_color = is_inactive ? Theme::chrome(*this).muted : (is_vertical ? Theme::chrome(*this).row_title : Theme::chrome(*this).tab_title);
        auto selected_text_color = is_inactive ? Theme::chrome(*this).muted : Theme::chrome(*this).text;
        auto tab_text_color = ChromeStyle::mix(unselected_text_color, selected_text_color, selection);

        // NB: A title keeps its weight whether its tab is selected or not, so nothing reflows as only its color
        //     cross-fades.
        auto paint_title = [&] {
            painter.setFont(title_font);

            auto title_text = m_tab_title_texts.value(title);
            if (title_text.text().isEmpty() && !title.isEmpty()) {
                auto single_line_title = title;
                single_line_title.replace(QLatin1Char('\n'), QLatin1Char(' '));
                title_text.setText(single_line_title);
                title_text.setTextFormat(Qt::PlainText);
                title_text.prepare(QTransform(), title_font);
            }
            title_texts.insert(title, title_text);

            // NB: A static text's height includes the font's leading, but a title is centered on its ascent and descent.
            auto title_size = title_text.size();
            QPointF title_position(contents_rect.left(), contents_rect.top() + ((contents_rect.height() - title_line_height) / 2));

            painter.save();
            painter.setPen(tab_text_color);
            painter.setOpacity(is_vertical && m_tab_widget ? m_tab_widget->vertical_tab_title_opacity() : 1.0);

            // A title too long for its tab fades out toward the tab's end, rather than being cut short with an ellipsis.
            // NB: Compare in fractional pixels, so a title that just fits is not taken for one that does not.
            if (auto* tab = m_tab_widget ? m_tab_widget->tab(index) : nullptr; tab && title_size.width() > contents_rect.width())
                tabs_with_cut_titles.set(tab);
            if (title_size.width() > contents_rect.width()) {
                QRect fade_rect(contents_rect.right() + 1 - TAB_TITLE_FADE_WIDTH, contents_rect.top(), TAB_TITLE_FADE_WIDTH, contents_rect.height());
                // The fading end gets a clip of its own below, which intersects the tab's clip rather than this one.
                painter.save();
                painter.setClipRect(contents_rect.adjusted(0, 0, -TAB_TITLE_FADE_WIDTH, 0), Qt::IntersectClip);
                painter.drawStaticText(title_position, title_text);
                painter.restore();

                // NB: Qt draws text with a gradient pen far more slowly than with a solid one, so only the fading end is
                //     drawn offscreen and faded out there.
                auto device_pixel_ratio = devicePixelRatioF();
                // NB: Round up, so the image covers all of the fade at a fractional pixel ratio. It is clipped to it.
                QImage fade_image(QSize(ceil(fade_rect.width() * device_pixel_ratio), ceil(fade_rect.height() * device_pixel_ratio)), QImage::Format_ARGB32_Premultiplied);
                fade_image.setDevicePixelRatio(device_pixel_ratio);
                fade_image.fill(Qt::transparent);
                {
                    QPainter fade_painter(&fade_image);
                    fade_painter.setFont(title_font);
                    fade_painter.setPen(tab_text_color);
                    fade_painter.drawStaticText(title_position - QPointF(fade_rect.topLeft()), title_text);

                    QLinearGradient fade(0, 0, fade_rect.width(), 0);
                    fade.setColorAt(0.0, Qt::black);
                    fade.setColorAt(1.0, Qt::transparent);
                    fade_painter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
                    fade_painter.fillRect(QRect(QPoint(), fade_rect.size()), fade);
                }
                // NB: A title narrower than the fade starts fading at its start, never over the icon before it.
                painter.setClipRect(fade_rect.intersected(contents_rect), Qt::IntersectClip);
                painter.drawImage(fade_rect.topLeft(), fade_image);
            } else {
                painter.setClipRect(contents_rect, Qt::IntersectClip);
                painter.drawStaticText(title_position, title_text);
            }
            painter.restore();
        };
        paint_title();
    }

    m_tab_title_texts = AK::move(title_texts);
    m_tabs_with_cut_titles = AK::move(tabs_with_cut_titles);

    painter.setClipping(false);
    // While the strip scrolls, it fades out at each end that has more tabs beyond it.
    if (!is_vertical) {
        auto strip = ChromeStyle::chrome_tab_strip_background(Theme::chrome_palette(*this));
        auto paint_fade = [&](int x, bool fades_right) {
            QLinearGradient fade(x, 0, x + HORIZONTAL_TAB_STRIP_FADE_WIDTH, 0);
            auto transparent_strip = strip;
            transparent_strip.setAlpha(0);
            fade.setColorAt(fades_right ? 0.0 : 1.0, transparent_strip);
            fade.setColorAt(fades_right ? 1.0 : 0.0, strip);
            painter.fillRect(QRect(x, 0, HORIZONTAL_TAB_STRIP_FADE_WIDTH, height()), fade);
        };
        if (m_scroll_offset > 0)
            paint_fade(0, false);
        if (m_scroll_offset < max_scroll_offset())
            paint_fade(width() - HORIZONTAL_TAB_STRIP_FADE_WIDTH, true);
    }
    if (is_vertical && (region.count > 0 || pinned_region_growth > 0)) {
        painter.setPen(Theme::chrome(*this).separator);
        auto separator_y = VERTICAL_TAB_BAR_END_ROOM + region.height + pinned_region_growth - (PINNED_TABS_SEPARATOR_HEIGHT / 2) - (region.scrolls ? m_scroll_offset : 0);
        painter.drawLine(QPoint(10, separator_y), QPoint(width() - 10, separator_y));
    }
}

void TabBar::contextMenuEvent(QContextMenuEvent* event)
{
    if (!m_tab_widget)
        return;

    auto tab_index = tab_index_at(event->pos());
    if (tab_index < 0) {
        show_window_context_menu(*this, event->globalPos());
        return;
    }

    m_pressed_tab = nullptr;
    if (auto* tab = m_tab_widget->tab(tab_index))
        execute_context_menu(*tab->context_menu(), event->globalPos());
}

void TabBar::dragEnterEvent(QDragEnterEvent* event)
{
    if (!m_tab_widget || !target_can_accept_tab_drop(*m_tab_widget, *event)) {
        event->ignore();
        return;
    }

    event->setDropAction(Qt::MoveAction);
    event->accept();
}

void TabBar::dragLeaveEvent(QDragLeaveEvent* event)
{
    hide_tab_preview();
    hide_drop_preview();
    QTabBar::dragLeaveEvent(event);
}

void TabBar::dragMoveEvent(QDragMoveEvent* event)
{
    if (!m_tab_widget || !target_can_accept_tab_drop(*m_tab_widget, *event)) {
        hide_drop_preview();
        event->ignore();
        return;
    }

    update_drop_preview(event->position().toPoint());
    event->setDropAction(Qt::MoveAction);
    event->accept();
}

void TabBar::dropEvent(QDropEvent* event)
{
    hide_tab_preview();
    if (!m_tab_widget || !target_can_accept_tab_drop(*m_tab_widget, *event)) {
        hide_drop_preview();
        event->ignore();
        return;
    }

    s_pending_tab_drop_target = m_tab_widget;
    s_pending_tab_drop_index = drop_index_at(event->position().toPoint());

    // NB: The drop preview stays up until the tab has actually moved, so the other tabs never jump back to their old
    //     places first.
    event->setDropAction(Qt::MoveAction);
    event->accept();
}

bool TabBar::eventFilter(QObject* watched, QEvent* event)
{
    auto hovered_tab_for_button = [this](QObject* button) {
        for (int index = 0; index < count(); ++index) {
            if (tabButton(index, QTabBar::LeftSide) == button || tabButton(index, QTabBar::RightSide) == button)
                return index;
        }
        return -1;
    };

    if (auto hovered_tab = hovered_tab_for_button(watched); hovered_tab >= 0) {
        if (event->type() == QEvent::Enter || event->type() == QEvent::MouseMove) {
            set_hovered_tab_index(hovered_tab);
        } else if (event->type() == QEvent::Leave) {
            auto cursor_position = mapFromGlobal(QCursor::pos());
            set_hovered_tab_index(tab_index_at(cursor_position));
        }
    }

    return QTabBar::eventFilter(watched, event);
}

void TabBar::mousePressEvent(QMouseEvent* event)
{
    hide_tab_preview();
    auto pressed_tab = tab_index_at(event->pos());
    m_pressed_tab = (m_tab_widget && pressed_tab >= 0) ? m_tab_widget->tab(pressed_tab) : nullptr;

    if (pressed_tab >= 0) {
        auto rect_of_current_tab = visual_tab_rect(pressed_tab);
        m_position_in_selected_tab_while_dragging = event->pos() - rect_of_current_tab.topLeft();
        m_drag_start_position = event->pos();
    }

    // NB: QTabBar finds the pressed tab without our scroll offset, so we select it ourselves.
    if (pressed_tab >= 0 && event->button() == Qt::LeftButton) {
        setCurrentIndex(pressed_tab);
    } else if (event->button() == Qt::LeftButton && start_window_move()) {
        event->accept();
        return;
    }

    if (m_pressed_tab)
        event->accept();
    else
        event->ignore();
}

void TabBar::mouseMoveEvent(QMouseEvent* event)
{
    if (m_strip_drag_card) {
        update_strip_drag(event->pos());
        event->accept();
        return;
    }

    set_hovered_tab_index(tab_index_at(event->pos()));

    if (count() == 0) {
        if (tab_layout() == TabLayout::Horizontal)
            QTabBar::mouseMoveEvent(event);
        return;
    }

    if (m_pressed_tab && event->buttons().testFlag(Qt::LeftButton)) {
        auto pressed_tab_index = m_tab_widget ? m_tab_widget->index_of(m_pressed_tab) : -1;
        if (pressed_tab_index < 0) {
            m_pressed_tab = nullptr;
        } else if ((event->pos() - m_drag_start_position).manhattanLength() >= TAB_DRAG_START_DISTANCE) {
            start_tab_drag(pressed_tab_index);
            event->accept();
            return;
        }
    }

    if (m_pressed_tab)
        event->accept();
    else
        event->ignore();
}

void TabBar::enterEvent(QEnterEvent* event)
{
    m_tab_width_unlock_timer->stop();
    QTabBar::enterEvent(event);
}

void TabBar::leaveEvent(QEvent* event)
{
    // NB: The tab preview's window reaches over the tabs, to make room for its shadow. Though it lets the mouse through,
    //     the pointer leaves the tab bar for it while it is still over the tab bar, so where the pointer is is watched
    //     instead while the preview shows.
    if (m_tab_preview_popup && m_tab_preview_popup->isVisible() && rect().contains(mapFromGlobal(QCursor::pos()))) {
        QTabBar::leaveEvent(event);
        return;
    }

    pointer_left();
    QTabBar::leaveEvent(event);
}

void TabBar::pointer_left()
{
    if (m_locked_tab_width.has_value())
        m_tab_width_unlock_timer->start();

    hide_tab_preview();
    set_hovered_tab_index(-1);
}

void TabBar::mouseReleaseEvent(QMouseEvent* event)
{
    if (m_strip_drag_card && event->button() == Qt::LeftButton) {
        auto dragged_tab = s_active_tab_dragged_tab;
        auto drop_index = drop_index_at(event->pos());
        end_strip_drag();
        if (auto* source_window = qobject_cast<BrowserWindow*>(window()); source_window && dragged_tab) {
            if (auto current_index = source_window->tab_index(dragged_tab); current_index >= 0)
                source_window->move_tab_to_window(current_index, *source_window, drop_index);
        }
        clear_drop_preview();
        event->accept();
        return;
    }

    auto pressed_tab = m_pressed_tab;
    m_pressed_tab = nullptr;

    if (event->button() == Qt::MiddleButton && pressed_tab && m_tab_widget) {
        if (auto index = m_tab_widget->index_of(pressed_tab); index >= 0 && tab_index_at(event->pos()) == index) {
            lock_tab_widths();
            emit tabCloseRequested(index);
            event->accept();
            return;
        }
    }

    if (tab_layout() == TabLayout::Horizontal)
        QTabBar::mouseReleaseEvent(event);

    if (pressed_tab)
        event->accept();
}

void TabBar::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (window_has_chrome_in_titlebar(*this) && tab_index_at(event->pos()) < 0 && event->button() == Qt::LeftButton) {
        toggle_window_maximized();
        event->accept();
        return;
    }

    QTabBar::mouseDoubleClickEvent(event);
}

void TabBar::wheelEvent(QWheelEvent* event)
{
    // Scrolling moves the tabs when they overflow, and never switches tabs.
    if (max_scroll_offset() <= 0) {
        event->ignore();
        return;
    }

    auto old_offset = m_scroll_offset;
    auto is_horizontal = tab_layout() == TabLayout::Horizontal;
    // NB: On a horizontal strip, both a vertical wheel and a sideways swipe scroll the tabs.
    auto dominant_delta = [&](QPoint delta) {
        if (!is_horizontal)
            return delta.y();
        return abs(delta.x()) > abs(delta.y()) ? delta.x() : delta.y();
    };
    auto pixel_delta = dominant_delta(event->pixelDelta());
    auto angle_delta = dominant_delta(event->angleDelta());

    if (pixel_delta != 0) {
        set_scroll_offset(m_scroll_offset - pixel_delta);
    } else if (angle_delta != 0) {
        auto scroll_delta = angle_delta * (is_horizontal ? HORIZONTAL_TAB_MIN_WIDTH : VERTICAL_TAB_HEIGHT) / 120;
        if (scroll_delta == 0)
            scroll_delta = angle_delta > 0 ? 1 : -1;
        set_scroll_offset(m_scroll_offset - scroll_delta);
    }

    if (m_scroll_offset != old_offset)
        event->accept();
    else
        event->ignore();
}

Optional<int> TabBar::dragged_tab_index() const
{
    if (!m_tab_widget || s_active_tab_drag_source != m_tab_widget || !s_active_tab_dragged_tab || s_dragged_tab_has_own_window)
        return {};
    if (auto index = m_tab_widget->index_of(s_active_tab_dragged_tab); index >= 0)
        return index;
    return {};
}

// A tab dragged in from elsewhere needs a gap of its own. Vertical tabs make room by moving the tabs after the gap
// down, while horizontal tabs narrow to fit the gap within the strip's current width.
bool TabBar::dragging_pinned_tab() const
{
    return s_active_tab_dragged_tab && s_active_tab_dragged_tab->is_pinned();
}

// Where the tab at a position sits once a tab dragged in from another window joins the dragged tab's own group. The
// pinned tiles keep their width, and the unpinned tabs narrow to make room, so the strip keeps its width.
QRect TabBar::incoming_drop_layout_rect(int position) const
{
    if (count() == 0)
        return {};

    auto pinned_count = pinned_tab_count() + (dragging_pinned_tab() ? 1 : 0);
    if (tab_layout() != TabLayout::Horizontal)
        return vertical_tab_rect(position, pinned_region_for(pinned_count));

    auto unpinned_count = count() + 1 - pinned_count;
    auto first_tab_rect = visual_tab_rect(0);
    auto strip_width = visual_tab_rect(count() - 1).right() + 1 - first_tab_rect.left() - (2 * HORIZONTAL_TAB_END_ROOM);
    auto unpinned_width = unpinned_count > 0 ? max(0.0, static_cast<qreal>(strip_width - (pinned_count * HORIZONTAL_PINNED_TAB_WIDTH)) / unpinned_count) : 0.0;

    auto left = first_tab_rect.left() + HORIZONTAL_TAB_END_ROOM + (min(position, pinned_count) * HORIZONTAL_PINNED_TAB_WIDTH) + (max(0, position - pinned_count) * unpinned_width);
    auto width = position < pinned_count ? HORIZONTAL_PINNED_TAB_WIDTH : unpinned_width;
    // NB: The first and last places also take room for their feet at the ends of the tab bar.
    if (position == 0) {
        left -= HORIZONTAL_TAB_END_ROOM;
        width += HORIZONTAL_TAB_END_ROOM;
    }
    if (position == count())
        width += HORIZONTAL_TAB_END_ROOM;
    return QRectF { left, static_cast<qreal>(first_tab_rect.top()), width, static_cast<qreal>(first_tab_rect.height()) }.toRect();
}

// Returns the index that a tab dropped at the given position ends up at.
int TabBar::drop_slot_at(QPoint const& position) const
{
    if (count() == 0)
        return 0;

    // A drag never moves a tab across the boundary between the pinned and unpinned tabs, so only the dragged tab's own
    // group has slots for it. Of those, the slot is the one whose place is nearest.
    auto pinned_count = pinned_tab_count();
    auto dragging_pinned = dragging_pinned_tab();
    auto nearest_slot = [](int first_slot, int last_slot, QPointF point, auto const& rect_for_slot) {
        auto best_slot = first_slot;
        auto best_distance = NumericLimits<qreal>::max();
        for (auto slot = first_slot; slot <= last_slot; ++slot) {
            auto offset = QRectF(rect_for_slot(slot)).center() - point;
            if (auto distance = QPointF::dotProduct(offset, offset); distance < best_distance) {
                best_distance = distance;
                best_slot = slot;
            }
        }
        return best_slot;
    };

    if (auto dragged_index = dragged_tab_index(); dragged_index.has_value()) {
        auto dragged_tab_rect = visual_tab_rect(*dragged_index);
        if (dragged_tab_rect.isEmpty())
            return *dragged_index;

        // Follow the center of the dragged tab rather than the cursor, so that a neighbor makes way once the dragged
        // tab covers half of it.
        auto center = QRectF(QPointF(position - m_position_in_selected_tab_while_dragging), QSizeF(dragged_tab_rect.size())).center();
        auto first_slot = dragging_pinned ? 0 : pinned_count;
        auto last_slot = dragging_pinned ? pinned_count - 1 : count() - 1;
        return nearest_slot(first_slot, last_slot, center, [this](int slot) { return visual_tab_rect(slot); });
    }

    auto first_slot = dragging_pinned ? 0 : pinned_count;
    auto last_slot = dragging_pinned ? pinned_count : count();
    return nearest_slot(first_slot, last_slot, QPointF(position), [this](int slot) { return incoming_drop_layout_rect(slot); });
}

// Returns the index to insert a tab dropped at the given position at. It counts the dragged tab in its old place, as
// moving it out of the way comes first.
int TabBar::drop_index_at(QPoint const& position) const
{
    auto drop_slot = drop_slot_at(position);
    auto dragged_index = dragged_tab_index();
    return dragged_index.has_value() && drop_slot > *dragged_index ? drop_slot + 1 : drop_slot;
}

void TabBar::update_drop_preview(QPoint const& position)
{
    set_drop_slot(drop_slot_at(position));
}

void TabBar::hide_drop_preview()
{
    set_drop_slot({});
}

void TabBar::set_drop_slot(Optional<int> slot)
{
    if (m_drop_slot == slot)
        return;

    m_drop_slot = slot;
    m_drop_preview_start = current_drop_preview();
    m_drop_preview_target = drop_preview_for_slot(slot);
    m_drop_preview_progress = 0.0;
    m_drop_preview_animation->stop();
    // With reduced motion, the other tabs jump into place.
    m_drop_preview_animation->setDuration(SystemAppearance::prefers_reduced_motion() ? 0 : DROP_PREVIEW_DURATION_MS);
    m_drop_preview_animation->start();
}

void TabBar::clear_drop_preview()
{
    m_drop_preview_animation->stop();
    m_drop_slot = {};
    m_drop_preview_start = {};
    m_drop_preview_target = {};
    m_drop_preview_progress = 1.0;
    updateGeometry();
    update_tab_button_geometry();
    update();
}

TabBar::DropPreview TabBar::drop_preview_for_slot(Optional<int> slot) const
{
    DropPreview preview;
    preview.tab_shifts.resize(count());
    if (!slot.has_value())
        return preview;

    auto move_to = [&](int index, QRect const& target) {
        auto tab_rect = visual_tab_rect(index);
        preview.tab_shifts[index] = {
            .x_offset = static_cast<qreal>(target.left() - tab_rect.left()),
            .y_offset = static_cast<qreal>(target.top() - tab_rect.top()),
            .width_delta = static_cast<qreal>(target.width() - tab_rect.width()),
            .height_delta = static_cast<qreal>(target.height() - tab_rect.height()),
        };
    };

    // The tabs between the dragged tab and its drop slot move over by one place to fill the dragged tab's place.
    if (auto dragged_index = dragged_tab_index(); dragged_index.has_value()) {
        for (int index = *dragged_index + 1; index <= *slot; ++index)
            move_to(index, visual_tab_rect(index - 1));
        for (int index = *slot; index < *dragged_index; ++index)
            move_to(index, visual_tab_rect(index + 1));
        return preview;
    }

    // A tab dragged in from another window opens a gap at its slot.
    for (int index = 0; index < count(); ++index)
        move_to(index, incoming_drop_layout_rect(index < *slot ? index : index + 1));
    if (tab_layout() != TabLayout::Horizontal) {
        auto region = pinned_region();
        auto incoming_region = pinned_region_for(region.count + (dragging_pinned_tab() ? 1 : 0));
        preview.pinned_region_growth = incoming_region.height - region.height;
        preview.strip_growth = preview.pinned_region_growth + (dragging_pinned_tab() ? 0 : VERTICAL_TAB_HEIGHT);
    }
    return preview;
}

TabBar::DropPreview TabBar::current_drop_preview() const
{
    DropPreview preview;
    for (int index = 0; index < count(); ++index)
        preview.tab_shifts.append(current_tab_shift(index));
    preview.strip_growth = current_strip_growth();
    preview.pinned_region_growth = current_pinned_region_growth();
    return preview;
}

TabBar::TabShift TabBar::current_tab_shift(int index) const
{
    auto shift_in = [index](DropPreview const& preview) {
        if (index < 0 || static_cast<size_t>(index) >= preview.tab_shifts.size())
            return TabShift {};
        return preview.tab_shifts[index];
    };

    auto start = shift_in(m_drop_preview_start);
    auto target = shift_in(m_drop_preview_target);
    auto interpolate = [this](qreal from, qreal to) { return from + ((to - from) * m_drop_preview_progress); };
    return {
        .x_offset = interpolate(start.x_offset, target.x_offset),
        .y_offset = interpolate(start.y_offset, target.y_offset),
        .width_delta = interpolate(start.width_delta, target.width_delta),
        .height_delta = interpolate(start.height_delta, target.height_delta),
    };
}

qreal TabBar::current_strip_growth() const
{
    auto start = m_drop_preview_start.strip_growth;
    return start + ((m_drop_preview_target.strip_growth - start) * m_drop_preview_progress);
}

qreal TabBar::current_pinned_region_growth() const
{
    auto start = m_drop_preview_start.pinned_region_growth;
    return start + ((m_drop_preview_target.pinned_region_growth - start) * m_drop_preview_progress);
}

// Returns where a tab is drawn, which differs from its place in the layout while tabs move aside for a dragged tab.
void TabBar::update_layout_animation()
{
    if (!m_tab_widget || tab_layout() != TabLayout::Horizontal)
        return;

    HashMap<Tab const*, QRect> targets;
    HashMap<Tab const*, HorizontalTabEndRoom> target_end_rooms;
    for (int index = 0; index < count(); ++index) {
        if (auto* tab = m_tab_widget->tab(index)) {
            targets.set(tab, tabRect(index));
            target_end_rooms.set(tab, settled_horizontal_tab_end_room(index, count()));
        }
    }

    // Only opening and closing tabs move the others. Resizing the window or a title changing does not.
    auto tabs_were_added_or_removed = count() != m_settled_tab_count && m_settled_tab_count > 0;
    auto can_animate = isVisible() && !dragged_tab_index().has_value() && !m_drop_slot.has_value() && !SystemAppearance::prefers_reduced_motion();
    // NB: A closing tab shrinks away only while the tabs move to close the gap it leaves.
    if (tabs_were_added_or_removed && !(count() < m_settled_tab_count && can_animate))
        m_closing_tab.clear();
    if (tabs_were_added_or_removed && can_animate) {
        HashMap<Tab const*, QRect> start_rects;
        HashMap<Tab const*, HorizontalTabEndRoom> start_end_rooms;
        for (auto const& [tab, target] : targets) {
            // A tab starts where it is drawn now, or as nothing at its place if it is new.
            auto settled = m_settled_tab_rects.get(tab);
            if (!settled.has_value()) {
                start_rects.set(tab, QRect { target.left(), target.top(), 0, target.height() });
                start_end_rooms.set(tab, *target_end_rooms.get(tab));
                continue;
            }
            // NB: A tab that becomes or stops being the first or last one gains or loses the room at the end of the strip
            //     as it moves, so what of it is drawn stays in place.
            auto settled_end_room = m_settled_tab_end_rooms.get(tab).value_or({});
            auto start_end_room = m_layout_animation_start_end_rooms.get(tab);
            if (m_layout_animation_progress < 1.0 && start_end_room.has_value()) {
                auto p = m_layout_animation_progress;
                start_end_rooms.set(tab, { start_end_room->left + ((settled_end_room.left - start_end_room->left) * p), start_end_room->right + ((settled_end_room.right - start_end_room->right) * p) });
            } else {
                start_end_rooms.set(tab, settled_end_room);
            }
            auto start = m_layout_animation_start_rects.get(tab);
            if (m_layout_animation_progress < 1.0 && start.has_value()) {
                auto p = m_layout_animation_progress;
                auto mix = [p](int a, int b) { return qRound(a + ((b - a) * p)); };
                start_rects.set(tab, QRect { mix(start->left(), settled->left()), mix(start->top(), settled->top()), mix(start->width(), settled->width()), mix(start->height(), settled->height()) });
            } else {
                start_rects.set(tab, *settled);
            }
        }
        m_layout_animation_start_rects = AK::move(start_rects);
        m_layout_animation_start_end_rooms = AK::move(start_end_rooms);
        m_layout_animation_progress = 0.0;
        m_layout_animation->stop();
        m_layout_animation->start();
    }

    m_settled_tab_rects = AK::move(targets);
    m_settled_tab_end_rooms = AK::move(target_end_rooms);
    m_settled_tab_count = count();
}

QRect TabBar::animated_tab_rect(int index, QRect const& target) const
{
    if (m_layout_animation_progress >= 1.0 || !m_tab_widget || tab_layout() != TabLayout::Horizontal)
        return target;
    auto* tab = m_tab_widget->tab(index);
    if (!tab)
        return target;
    auto start = m_layout_animation_start_rects.get(tab);
    if (!start.has_value())
        return target;

    // NB: The start is where the tab was in the tab bar, before any scrolling.
    auto from = start->translated(-m_scroll_offset, 0);
    auto p = m_layout_animation_progress;
    auto mix = [p](int a, int b) { return qRound(a + ((b - a) * p)); };
    return { mix(from.left(), target.left()), mix(from.top(), target.top()), mix(from.width(), target.width()), mix(from.height(), target.height()) };
}

HorizontalTabEndRoom TabBar::horizontal_tab_end_room(int index) const
{
    auto settled = settled_horizontal_tab_end_room(index, count());
    if (m_layout_animation_progress >= 1.0 || !m_tab_widget)
        return settled;
    auto* tab = m_tab_widget->tab(index);
    if (!tab)
        return settled;
    auto start = m_layout_animation_start_end_rooms.get(tab);
    if (!start.has_value())
        return settled;
    auto p = m_layout_animation_progress;
    return { start->left + ((settled.left - start->left) * p), start->right + ((settled.right - start->right) * p) };
}

void TabBar::prepare_for_tab_removal(int index)
{
    m_closing_tab.clear();
    if (tab_layout() != TabLayout::Horizontal || !isVisible() || SystemAppearance::prefers_reduced_motion() || dragged_tab_index().has_value() || count() <= 1)
        return;
    auto rect = shifted_tab_rect(index);
    if (rect.isEmpty())
        return;

    auto device_pixel_ratio = devicePixelRatioF();
    QPixmap image(rect.size() * device_pixel_ratio);
    image.setDevicePixelRatio(device_pixel_ratio);
    image.fill(Qt::transparent);
    {
        // NB: The closing tab is pictured as it looks unselected, and without its buttons, so that only the tab the
        //     selection moves to looks selected as it fades out.
        QPainter painter(&image);
        m_tab_painted_unselected = index;
        render(&painter, QPoint(), QRegion(rect), {});
        m_tab_painted_unselected = -1;
    }
    m_closing_tab = ClosingTab { AK::move(image), rect.translated(m_scroll_offset, 0) };
}

// The closing tab shrinks toward its start as the tabs after it close the gap.
QRect TabBar::closing_tab_rect() const
{
    if (!m_closing_tab.has_value() || m_layout_animation_progress >= 1.0)
        return {};
    auto rect = m_closing_tab->rect.translated(-m_scroll_offset, 0);
    rect.setWidth(qRound(rect.width() * (1.0 - m_layout_animation_progress)));
    return rect;
}

bool TabBar::is_growing_horizontal_tab(int index, QRect const& tab_rect) const
{
    return tab_rect.width() < visual_tab_rect(index).width();
}

// How selected a tab looks, while the selection cross-fades from one tab to another.
qreal TabBar::selection_of(int index) const
{
    if (index == m_tab_painted_unselected)
        return 0.0;
    auto previously_selected_index = m_previously_selected_tab && m_tab_widget ? m_tab_widget->index_of(m_previously_selected_tab) : -1;
    if (index == currentIndex())
        return m_selection_animation->state() == QAbstractAnimation::Running ? m_selection_progress : 1.0;
    if (index == previously_selected_index)
        return 1.0 - m_selection_progress;
    return 0.0;
}

// A horizontal tab's content is padded within its card, or within its body once it is selected. It moves between the
// two as the selection cross-fades.
QRectF TabBar::horizontal_tab_contents_rect(int index, QRect const& tab_rect) const
{
    // NB: A tab's contents keep their place as it is selected, while only its background grows around them.
    auto card = horizontal_tab_card_shape_rect(QRectF(tab_rect), horizontal_tab_end_room(index));
    return card.adjusted(HORIZONTAL_TAB_CONTENT_LEFT_PADDING, 0, -HORIZONTAL_TAB_CONTENT_RIGHT_PADDING, 0);
}

int TabBar::horizontal_tabs_end_offset() const
{
    if (count() == 0 || m_layout_animation_progress >= 1.0)
        return 0;
    // NB: A tab that opens starts out as nothing, so the tab before it may end further along.
    auto animated_end = closing_tab_rect().isEmpty() ? NumericLimits<int>::min() : closing_tab_rect().right();
    auto settled_end = NumericLimits<int>::min();
    for (int index = 0; index < count(); ++index) {
        auto target = visual_tab_rect(index);
        animated_end = max(animated_end, animated_tab_rect(index, target).right());
        settled_end = max(settled_end, target.right());
    }
    return animated_end - settled_end;
}

QRect TabBar::shifted_tab_rect(int index) const
{
    auto tab_rect = animated_tab_rect(index, visual_tab_rect(index));
    auto shift = current_tab_shift(index);
    return {
        qRound(tab_rect.left() + shift.x_offset),
        qRound(tab_rect.top() + shift.y_offset),
        qRound(tab_rect.width() + shift.width_delta),
        qRound(tab_rect.height() + shift.height_delta),
    };
}

// The card a dragged tab is drawn as. A horizontal tab's card is the selected tab's body, which starts a little below
// the top of the tab, and leaves out the room for its foot at either end of the strip.
QRect TabBar::tab_drag_card_rect(int index) const
{
    auto tab_rect = visual_tab_rect(index);
    if (tab_layout() != TabLayout::Horizontal)
        return tab_rect;
    return horizontal_selected_tab_body_rect(QRectF(tab_rect), horizontal_tab_end_room(index)).toAlignedRect();
}

QPixmap TabBar::render_tab_drag_pixmap(int index) const
{
    auto tab_rect = tab_drag_card_rect(index);
    auto device_pixel_ratio = devicePixelRatioF();
    auto dark = Theme::is_dark(*this);

    // The card: the tab on the toolbar color, rounded all around and outlined.
    QPixmap card(tab_rect.size() * device_pixel_ratio);
    card.setDevicePixelRatio(device_pixel_ratio);
    card.fill(Qt::transparent);
    {
        QPainter painter(&card);
        painter.setRenderHint(QPainter::Antialiasing);
        QPainterPath shape;
        shape.addRoundedRect(QRectF(QPointF(), tab_rect.size()), HORIZONTAL_SELECTED_TAB_RADIUS, HORIZONTAL_SELECTED_TAB_RADIUS);
        painter.setClipPath(shape);
        painter.fillPath(shape, Theme::chrome(*this).toolbar);
        const_cast<TabBar&>(*this).render(&painter, QPoint(), QRegion(tab_rect), QWidget::DrawChildren);
        painter.setClipping(false);
        painter.setPen(QPen(dark ? QColor(255, 255, 255, 26) : QColor(0, 0, 0, 36), 1));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(QRectF(QPointF(), tab_rect.size()).adjusted(0.5, 0.5, -0.5, -0.5), HORIZONTAL_SELECTED_TAB_RADIUS - 0.5, HORIZONTAL_SELECTED_TAB_RADIUS - 0.5);
    }

    // The card casts a tight shadow and a wider, softer one beneath it.
    auto image_size = tab_rect.size() + QSize(2 * TAB_DRAG_IMAGE_MARGIN, 2 * TAB_DRAG_IMAGE_MARGIN);
    QPixmap lifted(image_size * device_pixel_ratio);
    lifted.setDevicePixelRatio(device_pixel_ratio);
    lifted.fill(Qt::transparent);
    struct Shadow {
        qreal blur;
        qreal offset;
        qreal alpha;
    };
    Array<Shadow, 2> const shadows { Shadow { 16, 6, dark ? 0.30 : 0.12 }, Shadow { 2, 1, dark ? 0.36 : 0.18 } };
    for (auto const& shadow : shadows) {
        QGraphicsScene scene;
        auto* item = scene.addPixmap(card);
        auto* effect = new QGraphicsDropShadowEffect;
        effect->setBlurRadius(ChromeStyle::drop_shadow_blur_radius(shadow.blur));
        effect->setOffset(0, shadow.offset);
        effect->setColor(QColor(0, 0, 0, qRound(shadow.alpha * 255)));
        item->setGraphicsEffect(effect);
        auto source = QRectF(-TAB_DRAG_IMAGE_MARGIN, -TAB_DRAG_IMAGE_MARGIN, image_size.width(), image_size.height());
        QPainter painter(&lifted);
        painter.setRenderHint(QPainter::Antialiasing);
        scene.render(&painter, QRectF(QPointF(), image_size), source);
    }
    return lifted;
}

void TabBar::start_tab_drag(int index)
{
    hide_tab_preview();

    if (!m_tab_widget || !m_tab_widget->tab(index))
        return;

    start_strip_drag(index);
}

// The tab comes loose from its strip and follows the pointer freely, until it is dropped on a tab strip or, anywhere
// else, opens in a window of its own.
void TabBar::drag_tab_loose(Tab& tab, QPixmap card)
{
    hide_drop_preview();

    QPointer<Tab> dragged_tab = &tab;
    auto* drag = new QDrag(this);
    auto* mime_data = new QMimeData;
    mime_data->setData(LADYBIRD_TAB_MIME_TYPE, QByteArray {});
    drag->setMimeData(mime_data);
    drag->setPixmap(card);
    drag->setHotSpot(tab_drag_hot_spot());

    s_active_tab_drag_source = m_tab_widget;
    s_active_tab_dragged_tab = dragged_tab;
    s_pending_tab_drop_target = nullptr;
    s_pending_tab_drop_index = -1;
    update_tab_button_geometry();
    update();

    TabDragCancellationFilter cancellation_filter;

    // Qt installs its own drag event filter in exec(). Install ours afterward so Escape is observed before Qt consumes it.
    QTimer::singleShot(0, &cancellation_filter, [&cancellation_filter]() {
        QApplication::instance()->installEventFilter(&cancellation_filter);
    });

#if defined(AK_OS_MACOS)
    auto action = execute_appkit_tab_drag(*this, *drag);
#else
    auto action = drag->exec(Qt::MoveAction, Qt::MoveAction);
#endif
    QApplication::instance()->removeEventFilter(&cancellation_filter);

    // NB: On Wayland, a drag dropped where no window takes it ends just as one canceled with Escape does, and the
    //     compositor keeps the Escape key to itself. A tab dropped away from every tab strip is detached, as it is
    //     elsewhere, so a drag canceled there with Escape detaches its tab too.
    auto was_canceled = cancellation_filter.was_canceled();

    auto drop_target = s_pending_tab_drop_target;

    if (auto* source_window = qobject_cast<BrowserWindow*>(window()); source_window && dragged_tab) {
        if (auto current_index = source_window->tab_index(dragged_tab); current_index >= 0) {
            if (action == Qt::MoveAction && drop_target) {
                if (auto* target_window = qobject_cast<BrowserWindow*>(drop_target->window()))
                    source_window->move_tab_to_window(current_index, *target_window, s_pending_tab_drop_index);
            } else if (!drop_target && !was_canceled && m_tab_widget->count() > 1) {
                source_window->detach_tab_to_new_window(current_index, QCursor::pos());
            }
        }
    }

    s_active_tab_drag_source = nullptr;
    s_active_tab_dragged_tab = nullptr;
    s_pending_tab_drop_target = nullptr;
    s_pending_tab_drop_index = -1;
    m_pressed_tab = nullptr;

    // The dropped tab has taken its place in the target's layout by now, so the tabs can go back to their own places.
    clear_drop_preview();
    if (drop_target && drop_target != m_tab_widget)
        drop_target->tab_bar()->clear_drop_preview();
}

QPoint TabBar::tab_drag_hot_spot() const
{
    return m_position_in_selected_tab_while_dragging - m_tab_drag_card_offset + QPoint(TAB_DRAG_IMAGE_MARGIN, TAB_DRAG_IMAGE_MARGIN);
}

void TabBar::start_strip_drag(int index)
{
    // NB: The card is drawn once, before the tab is dragged, since the strip leaves a gap where the dragged tab is, and
    //     the other tabs move about it from then on. It shows the tab fully selected, even if the drag started while
    //     its selection was still fading in.
    if (m_selection_animation->state() == QAbstractAnimation::Running) {
        m_selection_animation->stop();
        m_selection_progress = 1.0;
        if (m_layout_animation_progress >= 1.0)
            m_previously_selected_tab = nullptr;
    }
    m_tab_drag_pixmap = render_tab_drag_pixmap(index);
    m_tab_drag_card_offset = tab_drag_card_rect(index).topLeft() - visual_tab_rect(index).topLeft();
    m_strip_drag_card = new TabDragCard(*window(), m_tab_drag_pixmap);

    s_active_tab_drag_source = m_tab_widget;
    s_active_tab_dragged_tab = m_tab_widget->tab(index);
    s_pending_tab_drop_target = nullptr;
    s_pending_tab_drop_index = -1;

    m_strip_drag_card->show();
    m_strip_drag_card->raise();
    m_strip_drag_cancellation_filter = new TabDragCancellationFilter([this] {
        end_strip_drag();
        clear_drop_preview();
    });
    QApplication::instance()->installEventFilter(m_strip_drag_cancellation_filter);

    update_tab_button_geometry();
    update();
    update_strip_drag(mapFromGlobal(QCursor::pos()));
}

void TabBar::update_strip_drag(QPoint position)
{
    auto dragged_tab = s_active_tab_dragged_tab;
    if (!dragged_tab || m_tab_widget->index_of(dragged_tab) < 0) {
        end_strip_drag();
        clear_drop_preview();
        return;
    }

    if (!m_tab_widget->tab_drop_area_contains(*this, position)) {
        auto card = m_tab_drag_pixmap;
        end_strip_drag();
        if (compositor_moves_dragged_windows())
            drag_tab_in_own_window(*dragged_tab);
        else
            drag_tab_loose(*dragged_tab, AK::move(card));
        return;
    }

    // The tab stays in its strip, and only slides along it.
    auto card_rect = tab_drag_card_rect(m_tab_widget->index_of(dragged_tab));
    auto card_position = tab_layout() == TabLayout::Horizontal
        ? QPoint(position.x() - m_position_in_selected_tab_while_dragging.x() + m_tab_drag_card_offset.x(), card_rect.top())
        : QPoint(card_rect.left(), position.y() - m_position_in_selected_tab_while_dragging.y());
    m_strip_drag_card->move(mapTo(window(), card_position) - QPoint(TAB_DRAG_IMAGE_MARGIN, TAB_DRAG_IMAGE_MARGIN));
    update_drop_preview(position);
}

void TabBar::end_strip_drag()
{
    if (!m_strip_drag_card)
        return;

    delete m_strip_drag_card;
    m_strip_drag_card = nullptr;
    m_tab_drag_pixmap = {};
    // NB: This can run from within the filter itself, as Escape cancels the drag.
    m_strip_drag_cancellation_filter->deleteLater();
    QApplication::instance()->removeEventFilter(m_strip_drag_cancellation_filter);
    m_strip_drag_cancellation_filter = nullptr;

    s_active_tab_drag_source = nullptr;
    s_active_tab_dragged_tab = nullptr;
    m_pressed_tab = nullptr;
    update_tab_button_geometry();
    update();
}

// The tab leaves the strip in a window of its own, which the compositor moves with the pointer until it is dropped. A
// tab that is alone in its window takes the whole window along.
// NB: Qt attaches the window to the drag before it starts the drag, and Mutter only moves a window attached that early
//     once it is first shown. So there, a window that was already shown stays in place, though its tab still lands
//     wherever it is dropped.
void TabBar::drag_tab_in_own_window(Tab& tab)
{
    auto* source_window = qobject_cast<BrowserWindow*>(window());
    if (!source_window)
        return;

    hide_drop_preview();

    QPointer<Tab> dragged_tab = &tab;
    auto* dragged_window = source_window;
    if (m_tab_widget->count() > 1)
        dragged_window = &source_window->detach_tab_to_new_window(source_window->tab_index(&tab), QCursor::pos(), BrowserWindow::DetachedWindowFollowsPointer::Yes);

    auto& tab_widget = dragged_window->tab_widget();
    auto* tab_bar = tab_widget.tab_bar();
    auto tab_rect = tab_bar->visual_tab_rect(tab_widget.index_of(&tab));
    auto position_in_tab = QPoint(
        min(m_position_in_selected_tab_while_dragging.x(), tab_rect.width() - 1),
        min(m_position_in_selected_tab_while_dragging.y(), tab_rect.height() - 1));
    auto position_in_window = tab_bar->mapTo(dragged_window, tab_rect.topLeft() + position_in_tab);

    // NB: Qt has the compositor move the window this mime data names with the pointer, at this position in the window.
    QByteArray window_data;
    QDataStream window_stream(&window_data, QIODevice::WriteOnly);
    window_stream << reinterpret_cast<qintptr>(dragged_window->windowHandle());
    QByteArray position_data;
    QDataStream position_stream(&position_data, QIODevice::WriteOnly);
    position_stream << position_in_window;

    auto* mime_data = new QMimeData;
    mime_data->setData(LADYBIRD_TAB_MIME_TYPE, QByteArray {});
    mime_data->setData(QStringLiteral("application/x-qt-mainwindowdrag-window"), window_data);
    mime_data->setData(QStringLiteral("application/x-qt-mainwindowdrag-position"), position_data);

    // The window itself shows what is being dragged, so the drag has no image.
    QPixmap no_image(1, 1);
    no_image.fill(Qt::transparent);

    auto* drag = new QDrag(dragged_window);
    drag->setMimeData(mime_data);
    drag->setPixmap(no_image);

    s_active_tab_drag_source = &tab_widget;
    s_active_tab_dragged_tab = dragged_tab;
    s_dragged_tab_has_own_window = true;
    s_pending_tab_drop_target = nullptr;
    s_pending_tab_drop_index = -1;

    auto action = drag->exec(Qt::MoveAction, Qt::MoveAction);

    auto drop_target = s_pending_tab_drop_target;
    auto drop_index = s_pending_tab_drop_index;
    s_active_tab_drag_source = nullptr;
    s_active_tab_dragged_tab = nullptr;
    s_dragged_tab_has_own_window = false;
    s_pending_tab_drop_target = nullptr;
    s_pending_tab_drop_index = -1;

    // A tab dropped on another window's strip joins that window. Dropped anywhere else, it stays in its own window.
    // NB: Moving the window's last tab away closes the window, which may be the one this tab bar is in.
    if (action == Qt::MoveAction && drop_target && dragged_tab && drop_target != &tab_widget) {
        if (auto* target_window = qobject_cast<BrowserWindow*>(drop_target->window())) {
            if (auto current_index = dragged_window->tab_index(dragged_tab); current_index >= 0)
                dragged_window->move_tab_to_window(current_index, *target_window, drop_index);
        }
    }
    if (drop_target)
        drop_target->tab_bar()->clear_drop_preview();
}

void TabBar::start_hover_animation(int tab_index, qreal target_progress)
{
    if (!m_hover_animation)
        return;

    if (tab_index < 0) {
        m_hover_animation->stop();
        m_hover_animation_tab_index = -1;
        m_hover_progress = 0.0;
        update();
        return;
    }

    m_hover_animation->stop();
    auto start_progress = (m_hover_animation_tab_index == tab_index) ? m_hover_progress : 0.0;
    m_hover_animation_tab_index = tab_index;
    m_hover_animation->setStartValue(start_progress);
    m_hover_animation->setEndValue(target_progress);
    m_hover_animation->start();
}

qreal TabBar::loading_ring_opacity(int index) const
{
    if (!m_tab_widget || index < 0 || index >= count())
        return 0;
    auto* tab = m_tab_widget->tab(index);
    return tab ? tab->loading_ring_opacity() : 0;
}

// The ring only needs repainting while a tab is loading and the tab bar can be seen.
void TabBar::update_loading_ring_animation()
{
    auto any_tab_loading = false;
    for (int index = 0; index < count() && !any_tab_loading; ++index)
        any_tab_loading = loading_ring_opacity(index) > 0;

    auto should_run = any_tab_loading && isVisible();
    if (should_run && m_loading_ring_animation->state() != QAbstractAnimation::Running)
        m_loading_ring_animation->start();
    else if (!should_run && m_loading_ring_animation->state() != QAbstractAnimation::Stopped)
        m_loading_ring_animation->stop();
    update();
}

void TabBar::schedule_tab_preview(int index)
{
    if (!m_tab_preview_timer || !m_tab_preview_popup)
        return;

    if (!m_tab_widget || index < 0 || index == currentIndex() || index >= count() || m_pressed_tab) {
        hide_tab_preview();
        return;
    }

    if (m_tab_preview_index == index && m_tab_preview_popup->isVisible())
        return;

    hide_tab_preview();
    m_tab_preview_index = index;
    QToolTip::hideText();
    m_tab_preview_timer->start();
}

void TabBar::show_tab_preview()
{
    if (!m_tab_widget || !m_tab_preview_popup)
        return;

    auto index = m_tab_preview_index;
    if (index < 0 || index != m_hovered_tab_index || index == currentIndex() || index >= count()) {
        hide_tab_preview();
        return;
    }

    auto* tab = m_tab_widget->tab(index);
    if (!tab) {
        hide_tab_preview();
        return;
    }

    // A tab that hasn't loaded has no page to show, so its preview is its title and URL.
    if (!tab->is_loaded()) {
        m_tab_preview_popup->set_preview(palette(), tab->title(), qstring_from_ak_string(tab->url().serialize()), {});
        m_tab_preview_popup->move(tab_preview_position_for(index, m_tab_preview_popup->sizeHint()));
        m_tab_preview_popup->show();
        m_tab_preview_popup->raise();
        m_tab_preview_pointer_timer->start();
        return;
    }

    if (m_previewed_tab != tab) {
        m_previewed_tab = tab;
        m_tab_preview_paint_connection = connect(&tab->view(), &WebContentView::ready_to_paint, this, &TabBar::show_tab_preview);
        // NB: Background tabs may never have painted. Allow rendering while their preview is requested,
        //     without selecting the tab or showing its widget.
        tab->view().set_system_visibility_state(Web::HTML::VisibilityState::Visible);
    }

    auto thumbnail = tab->view().tab_preview_pixmap({ TAB_PREVIEW_THUMBNAIL_WIDTH, TAB_PREVIEW_THUMBNAIL_HEIGHT });
    if (!thumbnail.has_value())
        return;

    m_tab_preview_popup->set_preview(palette(), tab->title(), qstring_from_ak_string(tab->view().url().serialize()), *thumbnail);
    m_tab_preview_popup->move(tab_preview_position_for(index, m_tab_preview_popup->sizeHint()));
    m_tab_preview_popup->show();
    m_tab_preview_popup->raise();
    m_tab_preview_pointer_timer->start();
}

void TabBar::hide_tab_preview()
{
    disconnect(m_tab_preview_paint_connection);
    if (m_previewed_tab) {
        m_previewed_tab->view().set_system_visibility_state(m_previewed_tab->view().isVisible() ? Web::HTML::VisibilityState::Visible : Web::HTML::VisibilityState::Hidden);
        m_previewed_tab = nullptr;
    }

    if (m_tab_preview_timer)
        m_tab_preview_timer->stop();
    if (m_tab_preview_pointer_timer)
        m_tab_preview_pointer_timer->stop();

    m_tab_preview_index = -1;

    if (m_tab_preview_popup)
        m_tab_preview_popup->hide();
}

QPoint TabBar::tab_preview_position_for(int index, QSize const& popup_size) const
{
    auto tab_rect = visual_tab_rect(index);
    auto screen = QGuiApplication::screenAt(mapToGlobal(tab_rect.center()));
    if (!screen)
        screen = window()->screen();
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    auto available_geometry = screen ? screen->availableGeometry() : QRect {};

    QPoint position;
    if (tab_layout() == TabLayout::Horizontal) {
        position = mapToGlobal(QPoint {
            tab_rect.center().x() - popup_size.width() / 2,
            tab_rect.bottom() + TAB_PREVIEW_CARD_GAP - TAB_PREVIEW_SHADOW_MARGINS.top(),
        });

        if (position.y() + popup_size.height() > available_geometry.bottom())
            position.setY(mapToGlobal(QPoint { 0, tab_rect.top() }).y() - popup_size.height() - TAB_PREVIEW_CARD_GAP + TAB_PREVIEW_SHADOW_MARGINS.bottom());
    } else {
        auto place_popup_to_right = !vertical_tabs_are_on_right();
        // The popup sits beside the tab bar rather than the tab, so it never covers the other pinned tiles in a row.
        auto bar_rect = rect();
        position = mapToGlobal(QPoint {
            place_popup_to_right
                ? bar_rect.right() + TAB_PREVIEW_CARD_GAP - TAB_PREVIEW_SHADOW_MARGINS.left()
                : bar_rect.left() - popup_size.width() - TAB_PREVIEW_CARD_GAP + TAB_PREVIEW_SHADOW_MARGINS.right(),
            tab_rect.center().y() - popup_size.height() / 2,
        });

        if (place_popup_to_right && position.x() + popup_size.width() > available_geometry.right())
            position.setX(mapToGlobal(QPoint { bar_rect.left(), 0 }).x() - popup_size.width() - TAB_PREVIEW_CARD_GAP + TAB_PREVIEW_SHADOW_MARGINS.right());
        else if (!place_popup_to_right && position.x() < available_geometry.left())
            position.setX(mapToGlobal(QPoint { bar_rect.right(), 0 }).x() + TAB_PREVIEW_CARD_GAP - TAB_PREVIEW_SHADOW_MARGINS.left());
    }

    auto minimum_x = available_geometry.left() + TAB_PREVIEW_CARD_GAP - TAB_PREVIEW_SHADOW_MARGINS.left();
    auto maximum_x = available_geometry.right() - popup_size.width() - TAB_PREVIEW_CARD_GAP + TAB_PREVIEW_SHADOW_MARGINS.right();
    if (minimum_x <= maximum_x)
        position.setX(clamp(position.x(), minimum_x, maximum_x));

    auto minimum_y = available_geometry.top() + TAB_PREVIEW_CARD_GAP - TAB_PREVIEW_SHADOW_MARGINS.top();
    auto maximum_y = available_geometry.bottom() - popup_size.height() - TAB_PREVIEW_CARD_GAP + TAB_PREVIEW_SHADOW_MARGINS.bottom();
    if (minimum_y <= maximum_y)
        position.setY(clamp(position.y(), minimum_y, maximum_y));

    return position;
}

// The width of the tabs, which the bar reaches past on the page's side, where the selected tab joins the page's frame.
int TabBar::vertical_bar_width() const
{
    auto tab_width = vertical_tab_width(m_available_width, tab_layout());
    if (auto bar_width = width(); bar_width > 0)
        return min(bar_width, tab_width);
    return tab_width;
}

int TabBar::vertical_tabs_left() const
{
    return vertical_tabs_are_on_right() ? max(0, width() - vertical_bar_width()) : 0;
}

TabBar::PinnedRegion TabBar::pinned_region() const
{
    return pinned_region_for(pinned_tab_count());
}

TabBar::PinnedRegion TabBar::pinned_region_for(int pinned_count) const
{
    PinnedRegion region;
    if (tab_layout() == TabLayout::Horizontal)
        return region;

    region.count = pinned_count;
    if (region.count == 0)
        return region;

    auto bar_width = vertical_bar_width();
    if (!pinned_tabs_are_tiles()) {
        region.columns = 1;
        region.tile_width = bar_width;
    } else {
        // The tiles stretch to fill each row.
        region.columns = PINNED_TAB_COLUMNS;
        region.tile_width = bar_width / region.columns;
    }

    auto rows = (region.count + region.columns - 1) / region.columns;
    // NB: The sidebar keeps its open layout while it closes, and takes it at once as it opens, so the tiles move
    //     between their grid at the open sidebar's width and the column at the collapsed sidebar's width as it toggles.
    //     A tile that followed the sidebar's width on the way would grow before it shrinks.
    if (pinned_tabs_are_tiles() && m_tab_widget && m_tab_widget->vertical_tabs_are_toggling()) {
        region.collapsedness = m_tab_widget->vertical_tabs_toggle_collapsedness();
        region.tile_width = m_tab_widget->vertical_tabs_toggle_open_tab_width() / region.columns;
        region.collapsed_row_width = m_tab_widget->vertical_tabs_toggle_collapsed_tab_width();
        region.height = qRound(((rows + ((region.count - rows) * region.collapsedness)) * VERTICAL_TAB_HEIGHT)) + PINNED_TABS_SEPARATOR_HEIGHT;
    } else {
        region.height = (rows * VERTICAL_TAB_HEIGHT) + PINNED_TABS_SEPARATOR_HEIGHT;
    }
    region.scrolls = region.height > height() / 2;
    return region;
}

bool TabBar::pinned_tabs_are_tiles() const
{
    return tab_layout() == TabLayout::Horizontal || (tab_layout() == TabLayout::VerticalExpanded && !m_pinned_tabs_in_rows);
}

QRect TabBar::visual_tab_rect(int index) const
{
    if (tab_layout() == TabLayout::Horizontal)
        return tabRect(index).translated(-m_scroll_offset, 0);

    if (index < 0 || index >= count())
        return {};
    return vertical_tab_rect(index, pinned_region());
}

QRect TabBar::vertical_tab_rect(int index, PinnedRegion const& region) const
{
    // While the pinned tabs stay in place, only the unpinned list scrolls, under them.
    if (index < region.count) {
        auto row = index / region.columns;
        auto column = index % region.columns;
        auto scroll_offset = region.scrolls ? m_scroll_offset : 0;
        QRect tile { vertical_tabs_left() + (column * region.tile_width), VERTICAL_TAB_BAR_END_ROOM + (row * VERTICAL_TAB_HEIGHT) - scroll_offset, region.tile_width, VERTICAL_TAB_HEIGHT };
        if (region.collapsedness <= 0)
            return tile;
        QRect row_in_column { vertical_tabs_left(), VERTICAL_TAB_BAR_END_ROOM + (index * VERTICAL_TAB_HEIGHT) - scroll_offset, region.collapsed_row_width, VERTICAL_TAB_HEIGHT };
        auto mix = [&](int a, int b) { return qRound(a + ((b - a) * region.collapsedness)); };
        return { mix(tile.left(), row_in_column.left()), mix(tile.top(), row_in_column.top()), mix(tile.width(), row_in_column.width()), VERTICAL_TAB_HEIGHT };
    }

    auto unpinned_index = index - region.count;
    return { vertical_tabs_left(), VERTICAL_TAB_BAR_END_ROOM + region.height + (unpinned_index * VERTICAL_TAB_HEIGHT) - m_scroll_offset, vertical_bar_width(), VERTICAL_TAB_HEIGHT };
}

int TabBar::tab_index_at(QPoint const& position) const
{
    if (tab_layout() == TabLayout::Horizontal) {
        if (position.x() < 0 || position.x() >= width())
            return -1;
        auto index = tabAt(position + QPoint(m_scroll_offset, 0));
        // NB: The room for the selected tab's foot at either end of the strip is not part of the tab, so a press there
        //     drags the window, as one elsewhere on the empty strip does.
        if (index >= 0) {
            auto body = horizontal_selected_tab_body_rect(QRectF(visual_tab_rect(index)), horizontal_tab_end_room(index));
            if (position.x() < body.left() || position.x() >= body.right())
                return -1;
        }
        return index;
    }

    if (position.x() < 0 || position.x() >= width() || position.y() < 0 || position.y() >= height())
        return -1;

    auto region = pinned_region();
    auto y = position.y() - VERTICAL_TAB_BAR_END_ROOM;
    if (y < 0)
        return -1;
    auto pinned_y = y + (region.scrolls ? m_scroll_offset : 0);
    if (pinned_y < region.height) {
        if (pinned_y >= region.height - PINNED_TABS_SEPARATOR_HEIGHT)
            return -1;
        // NB: Tiles on their way between the grid and the column are found by where they are drawn. Where they overlap,
        //     the later tile is drawn on top, so it is found first.
        if (region.collapsedness > 0) {
            for (int index = region.count - 1; index >= 0; --index) {
                if (vertical_tab_rect(index, region).contains(position))
                    return index;
            }
            return -1;
        }
        auto x = position.x() - vertical_tabs_left();
        if (x < 0 || x >= region.columns * region.tile_width)
            return -1;
        auto row = pinned_y / VERTICAL_TAB_HEIGHT;
        auto column = min(x / region.tile_width, region.columns - 1);
        auto index = (row * region.columns) + column;
        return index < region.count ? index : -1;
    }

    auto index = region.count + ((y - region.height + m_scroll_offset) / VERTICAL_TAB_HEIGHT);
    if (index >= count())
        return -1;

    if (!visual_tab_rect(index).contains(position))
        return -1;

    return index;
}

QSize TabBar::vertical_size_hint(int tab_count) const
{
    auto region = pinned_region();
    return { vertical_tab_width(m_available_width, tab_layout()), (2 * VERTICAL_TAB_BAR_END_ROOM) + region.height + (max(0, tab_count - region.count) * VERTICAL_TAB_HEIGHT) };
}

bool TabBar::vertical_tabs_are_on_right() const
{
    return m_vertical_tabs_position == WebView::VerticalTabsPosition::Right;
}

int TabBar::max_scroll_offset() const
{
    if (tab_layout() == TabLayout::Horizontal)
        return count() > 0 ? max(0, tabRect(count() - 1).right() + 1 - width()) : 0;
    auto region = pinned_region();
    return max(0, (2 * VERTICAL_TAB_BAR_END_ROOM) + region.height + ((count() - region.count) * VERTICAL_TAB_HEIGHT) - height());
}

void TabBar::set_scroll_offset(int offset)
{
    auto clamped_offset = max(0, min(offset, max_scroll_offset()));
    if (m_scroll_offset == clamped_offset)
        return;

    m_scroll_offset = clamped_offset;
    update_tab_button_geometry();
    update();
}

void TabBar::ensure_tab_visible(int index)
{
    if (index < 0 || index >= count())
        return;

    if (tab_layout() == TabLayout::Horizontal) {
        // A tab scrolled into view clears the fade at the end it comes from, unless it is the first or the last.
        auto tab_rect = tabRect(index);
        auto left = tab_rect.left() - (index > 0 ? HORIZONTAL_TAB_STRIP_FADE_WIDTH : 0);
        auto right = tab_rect.right() + 1 + (index < count() - 1 ? HORIZONTAL_TAB_STRIP_FADE_WIDTH : 0);
        if (left < m_scroll_offset)
            set_scroll_offset(left);
        else if (right > m_scroll_offset + width())
            set_scroll_offset(right - width());
        return;
    }

    // A pinned tab that stays in place is always in view.
    auto region = pinned_region();
    if (index < region.count && !region.scrolls)
        return;

    // Where the tab sits in the part of the bar that scrolls.
    auto tab_top = index < region.count
        ? (index / region.columns) * VERTICAL_TAB_HEIGHT
        : region.height + ((index - region.count) * VERTICAL_TAB_HEIGHT) - region.fixed_height();
    auto tab_bottom = tab_top + VERTICAL_TAB_HEIGHT;
    auto visible_height = height() - (2 * VERTICAL_TAB_BAR_END_ROOM) - region.fixed_height();

    if (tab_top < m_scroll_offset)
        set_scroll_offset(tab_top);
    else if (tab_bottom > m_scroll_offset + visible_height)
        set_scroll_offset(tab_bottom - visible_height);
}

void TabBar::set_hovered_tab_index(int index)
{
    if (m_hovered_tab_index == index)
        return;

    m_hovered_tab_index = index;
    update_tab_button_geometry();
    // A tab's hover fill fades in, but disappears at once when the pointer leaves.
    start_hover_animation(index, 1.0);
    schedule_tab_preview(index);
    update();
}

void TabBar::update_tab_button_geometry()
{
    auto prepare_button = [&](QWidget* button, TabBarButton::Placement placement) {
        if (!button)
            return;
        button->installEventFilter(this);
        if (auto* tab_bar_button = qobject_cast<TabBarButton*>(button))
            tab_bar_button->set_placement(placement);
    };

    // The audio button takes the favicon's place, in the middle of a pinned tile's card.
    auto center_button_in = [](QWidget* button, QRectF const& card) {
        auto button_size = button->size().isEmpty() ? button->sizeHint() : button->size();
        auto center = card.center();
        QRect button_geometry { qRound(center.x() - (button_size.width() / 2.0)), qRound(center.y() - (button_size.height() / 2.0)), button_size.width(), button_size.height() };
        if (button->geometry() != button_geometry)
            button->setGeometry(button_geometry);
    };

    // The buttons of a horizontal tab are centered on its card, rather than on the whole tab.
    auto button_y_in = [](QWidget* button, QRectF const& card) {
        auto button_height = button->size().isEmpty() ? button->sizeHint().height() : button->height();
        return qRound(card.center().y() - (button_height / 2.0));
    };

    auto dragged_index = dragged_tab_index();

    if (tab_layout() == TabLayout::Horizontal) {
        auto pinned_count = pinned_tab_count();
        // Buttons are widgets above the tab bar's painting, so the fade at a scrolling end can't cover them. They hide
        // there instead.
        auto visible_left = m_scroll_offset > 0 ? HORIZONTAL_TAB_STRIP_FADE_WIDTH : 0;
        auto visible_right = width() - (m_scroll_offset < max_scroll_offset() ? HORIZONTAL_TAB_STRIP_FADE_WIDTH : 0);
        auto is_in_view = [&](QWidget* button) {
            return button->x() >= visible_left && button->x() + button->width() <= visible_right;
        };
        for (int index = 0; index < count(); ++index) {
            auto is_dragged = index == dragged_index;
            auto tab_rect = shifted_tab_rect(index);
            auto shape_rect = horizontal_tab_card_shape_rect(QRectF(tab_rect), horizontal_tab_end_room(index));
            auto* left_button = tabButton(index, QTabBar::LeftSide);
            prepare_button(left_button, TabBarButton::Placement::Tab);

            if (index < pinned_count) {
                if (!left_button)
                    continue;
                center_button_in(left_button, shape_rect);
                if (auto visible = !is_dragged && is_in_view(left_button); left_button->isVisible() != visible)
                    left_button->setVisible(visible);
                continue;
            }

            auto is_selected = index == currentIndex();
            auto content_rect = horizontal_tab_contents_rect(index, tab_rect);
            // NB: A tab shows its buttons once it has grown to its width.
            auto is_growing = is_growing_horizontal_tab(index, tab_rect);

            // The close button ends the tab's content, and only shows on the selected tab and the hovered one.
            auto* close_button = tabButton(index, QTabBar::RightSide);
            prepare_button(close_button, TabBarButton::Placement::Tab);
            auto trailing_edge = qRound(content_rect.right());
            if (close_button) {
                QPoint close_button_position { trailing_edge - close_button->width(), button_y_in(close_button, shape_rect) };
                if (close_button->pos() != close_button_position)
                    close_button->move(close_button_position);
                // NB: Every tab with room for it keeps a slot for its close button, which shows on the selected tab and
                //     the hovered one. While the strip scrolls, only the selected tab shows it. A tab too narrow for
                //     its favicon beside the slot, such as one opening or closing, has none.
                auto has_room = close_button_position.x() - HORIZONTAL_TAB_CONTENT_GAP >= content_rect.left() + TAB_ICON_SIZE;
                auto show_close_button = !is_dragged && !is_growing && has_room && (is_selected || (index == m_hovered_tab_index && max_scroll_offset() <= 0)) && is_in_view(close_button);
                if (auto* tab_bar_button = qobject_cast<TabBarButton*>(close_button))
                    tab_bar_button->set_revealed(show_close_button);
                else if (close_button->isVisible() != show_close_button)
                    close_button->setVisible(show_close_button);
                if (has_room)
                    trailing_edge = close_button_position.x() - 2;
            }

            // The speaker of a tab playing audio sits left of the close button's slot.
            if (left_button) {
                QPoint position { trailing_edge - left_button->width(), button_y_in(left_button, shape_rect) };
                if (left_button->pos() != position)
                    left_button->move(position);
                if (auto visible = !is_dragged && !is_growing && is_in_view(left_button); left_button->isVisible() != visible)
                    left_button->setVisible(visible);
            }
        }
        return;
    }

    // Keep this out of paintEvent(): setVisible(), setGeometry(), and raise()
    // dirty child widgets, and doing that while painting can schedule another
    // tab bar repaint before the current one has finished flushing.
    auto place_expanded_button = [&](int index, QTabBar::ButtonPosition position, QRect shape_rect) {
        auto* button = tabButton(index, position);
        if (!button)
            return;
        prepare_button(button, TabBarButton::Placement::SidebarRow);

        auto button_size = button->size();
        if (button_size.isEmpty())
            button_size = button->sizeHint();
        if (button_size.isEmpty())
            return;

        // The close button's circle shares its center with the round end of the card it sits in. The speaker sits
        // before the close button while it shows, and in its place otherwise.
        // NB: A QRect's right() is its last column, one short of its right edge.
        auto end_radius = shape_rect.height() / 2;
        auto x = shape_rect.right() + 1 - end_radius - (button_size.width() / 2);
        if (position == QTabBar::LeftSide) {
            if (auto* close_button = tabButton(index, QTabBar::RightSide); close_button && close_button->isVisible())
                x = close_button->x() - SIDEBAR_ROW_BUTTON_GAP - button_size.width();
        }

        // The expanded layout is also used at narrow widths during hover expansion. Keep buttons from covering the
        // favicon while the sidebar grows.
        auto icon_left = shape_rect.left() + VERTICAL_TAB_CONTENT_LEADING_INSET;
        auto has_room_for_icon = x - 6 >= icon_left + TAB_ICON_SIZE;

        // NB: Move a button before showing it. Shown where it was, under the pointer, it would take a hover that it
        //     keeps at its new place until the pointer moves.
        auto did_update_button = false;
        auto y = shape_rect.top() + ((shape_rect.height() - button_size.height()) / 2);
        QRect button_geometry { { x, y }, button_size };
        if (button->geometry() != button_geometry) {
            button->setGeometry(button_geometry);
            did_update_button = true;
        }

        // NB: The close buttons stay hidden while the sidebar opens or closes, as the titles fade.
        auto is_toggling = m_tab_widget && m_tab_widget->vertical_tabs_are_toggling();
        auto should_be_visible = index != dragged_index && has_room_for_icon
            && (position != QTabBar::RightSide || ((index == currentIndex() || index == m_hovered_tab_index) && !is_toggling));
        if (button->isVisible() != should_be_visible) {
            button->setVisible(should_be_visible);
            did_update_button = true;
        }

        if (did_update_button)
            button->raise();
    };

    auto place_collapsed_button = [&](int index, QTabBar::ButtonPosition position, QRect shape_rect) {
        auto* button = tabButton(index, position);
        if (!button)
            return;
        prepare_button(button, TabBarButton::Placement::RailBadge);

        auto button_size = button->size();
        if (button_size.isEmpty())
            button_size = button->sizeHint();
        if (button_size.isEmpty())
            return;

        // The close button sits on the favicon's top left corner, and the speaker on its bottom right corner.
        auto icon_center = QRectF(shape_rect).center();
        QPoint icon_top_left { qRound(icon_center.x()) - (TAB_ICON_SIZE / 2), qRound(icon_center.y()) - (TAB_ICON_SIZE / 2) };
        auto badge_center = position == QTabBar::RightSide
            ? icon_top_left - RAIL_BADGE_CORNER_OUTSET
            : icon_top_left + QPoint(TAB_ICON_SIZE, TAB_ICON_SIZE) + RAIL_BADGE_CORNER_OUTSET;
        auto button_position = badge_center - QPoint(button_size.width() / 2, button_size.height() / 2);

        // NB: As above, move the button before showing it.
        bool did_update_button = false;
        QRect button_geometry { button_position, button_size };
        if (button->geometry() != button_geometry) {
            button->setGeometry(button_geometry);
            did_update_button = true;
        }

        auto should_be_visible = index != dragged_index
            && (position == QTabBar::LeftSide || (index == m_hovered_tab_index && !m_tab_widget->can_expand_vertical_tabs_on_hover()));
        if (button->isVisible() != should_be_visible) {
            button->setVisible(should_be_visible);
            did_update_button = true;
        }

        if (did_update_button)
            button->raise();
    };

    auto region = pinned_region();
    for (int index = 0; index < count(); ++index) {
        auto tab_rect = shifted_tab_rect(index);
        if (!tab_rect.isValid())
            continue;

        // An unpinned tab scrolled under the pinned tabs keeps its buttons out of sight.
        if (region.count > 0 && !region.scrolls && index >= region.count && tab_rect.top() < region.height) {
            for (auto position : { QTabBar::LeftSide, QTabBar::RightSide }) {
                if (auto* button = tabButton(index, position); button && button->isVisible())
                    button->setVisible(false);
            }
            continue;
        }

        if (tab_layout() == TabLayout::VerticalCollapsed) {
            auto shape_rect = collapsed_vertical_tab_shape_rect(tab_rect);
            place_collapsed_button(index, QTabBar::LeftSide, shape_rect);
            place_collapsed_button(index, QTabBar::RightSide, shape_rect);
        } else if (index < region.count && pinned_tabs_are_tiles()) {
            if (auto* button = tabButton(index, QTabBar::LeftSide)) {
                prepare_button(button, TabBarButton::Placement::Tab);
                center_button_in(button, tab_rect);
                if (button->isVisible() != (index != dragged_index))
                    button->setVisible(index != dragged_index);
            }
        } else {
            auto shape_rect = tab_card_shape_rect(tab_rect);
            place_expanded_button(index, QTabBar::RightSide, shape_rect);
            place_expanded_button(index, QTabBar::LeftSide, shape_rect);
        }
    }
}

void TabBar::toggle_window_maximized()
{
    auto* top_level_window = window();
    if (top_level_window->isMaximized())
        top_level_window->showNormal();
    else
        top_level_window->showMaximized();
}

bool TabBar::start_window_move()
{
    auto* handle = window()->windowHandle();
    if (!handle)
        return false;
#if defined(AK_OS_MACOS)
    if (start_appkit_window_drag(*this))
        return true;
#endif
    return handle->startSystemMove();
}

TabWidget::TabWidget(WebView::IsPrivate is_private, QWidget* parent)
    : QWidget(parent)
{
    // NB: The window is marked as private before any chrome inside it is made.
    if (is_private == WebView::IsPrivate::Yes)
        Theme::mark_private_window(*window());
    if (auto* top_level_window = window(); top_level_window != this)
        top_level_window->installEventFilter(this);

    m_vertical_tabs_expanded_width = Application::settings().tab_settings().vertical_tabs_expanded_width.value_or(VERTICAL_TABS_DEFAULT_EXPANDED_WIDTH);

    m_tab_bar = new TabBar(this);
    setAcceptDrops(true);

    m_tab_bar->setDocumentMode(true);
    m_tab_bar->setElideMode(Qt::TextElideMode::ElideRight);
    m_tab_bar->setMovable(false);
    m_tab_bar->setTabsClosable(true);
    m_tab_bar->setExpanding(false);
    m_tab_bar->setUsesScrollButtons(false);
    m_tab_bar->setDrawBase(false);
    m_tab_bar->installEventFilter(this);

    m_toolbar_container = new QStackedWidget(this);
    m_toolbar_container->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    m_toolbar_container->installEventFilter(this);
    m_stacked_widget = new QStackedWidget(this);
    m_page_column = new QWidget(this);
    m_page_column->installEventFilter(this);
    m_toolbar_placeholder = new QWidget(m_page_column);
    m_page_column_layout = new QVBoxLayout(m_page_column);
    m_page_column_layout->setSpacing(0);
    m_page_column_layout->setContentsMargins(0, 0, 0, 0);

    m_ladybird_button = new LadybirdButton(is_private, this);
    m_vertical_tabs_expanded_width = clamp(m_vertical_tabs_expanded_width, minimum_vertical_tabs_expanded_width(), VERTICAL_TABS_MAX_EXPANDED_WIDTH);

    m_new_tab_button = new NewTabButton(*m_tab_bar, this);
    m_new_tab_button->setObjectName("LadybirdNewTabButton");
    m_new_tab_button->setFixedSize(32, 32);
    m_new_tab_button->setFocusPolicy(Qt::NoFocus);
    m_new_tab_button->setToolTip(qformatted("New Tab  {}", QKeySequence(QKeySequence::AddTab).toString(QKeySequence::NativeText)));
    m_new_tab_button->installEventFilter(this);

    // Once the tabs overflow the strip, a button before the new tab button lists them all.
    m_all_tabs_button = new NewTabButton(*m_tab_bar, this);
    m_all_tabs_button->setObjectName("LadybirdAllTabsButton");
    m_all_tabs_button->setProperty(VERTICAL_TABS_BUTTON_PROPERTY, true);
    m_all_tabs_button->setIconSize(QSize(16, 16));
    m_all_tabs_button->setFixedSize(HORIZONTAL_NEW_TAB_BUTTON_SIZE, HORIZONTAL_NEW_TAB_BUTTON_SIZE);
    m_all_tabs_button->setFocusPolicy(Qt::NoFocus);
    m_all_tabs_button->setToolTip("Show All Tabs");
    m_all_tabs_button->hide();
    QObject::connect(m_all_tabs_button, &QToolButton::clicked, this, [this] { show_all_tabs(); });

    if constexpr (use_native_macos_window_controls()) {
        m_window_controls = create_window_controls_spacer(*this, "LadybirdTabStripWindowControls", { NATIVE_MACOS_WINDOW_CONTROLS_WIDTH, HORIZONTAL_TAB_HEIGHT });
    } else {
        auto window_control_buttons = create_window_control_buttons(*this, "LadybirdTabStripWindowControls", { 18, 18 }, { 46, HORIZONTAL_WINDOW_CONTROL_HEIGHT });
        m_window_controls = window_control_buttons.container;
        m_minimize_window_button = window_control_buttons.minimize;
        m_maximize_window_button = window_control_buttons.maximize;
        m_close_window_button = window_control_buttons.close;
    }

    recreate_icons();

    m_tab_bar_row = new QWidget(this);
    m_tab_bar_row->setObjectName("LadybirdTabStrip");
    m_tab_bar_row->setAcceptDrops(true);
    m_tab_bar_row_layout = new QHBoxLayout(m_tab_bar_row);
    m_tab_bar_row->installEventFilter(this);
    // NB: The tab bar is given a fixed width that fills the row whenever there are enough tabs, so the row's minimum
    //     width would be the window's current width. Keep it out of the window's minimum size, or the window could only
    //     be narrowed by the row's few spare pixels at each step of a resize. The tab bar is fitted to the new width,
    //     and update_tab_layout() gives the row the minimum width its controls need.
    m_tab_bar_row->setSizePolicy(QSizePolicy::Ignored, m_tab_bar_row->sizePolicy().verticalPolicy());

    m_vertical_tabs_content = new QWidget(this);
    m_vertical_tabs_content_layout = new QHBoxLayout(m_vertical_tabs_content);
    m_vertical_tabs_content_layout->setSpacing(0);
    m_vertical_tabs_content_layout->setContentsMargins(0, 0, 0, 0);
    m_vertical_tabs_content->installEventFilter(this);

    m_vertical_tab_bar_column = new QWidget(this);
    m_vertical_tab_bar_column->setObjectName("LadybirdVerticalTabBar");
    m_vertical_tab_bar_column->setAcceptDrops(true);
#if defined(AK_OS_MACOS)
    m_vertical_tab_bar_column->setAttribute(Qt::WA_DontCreateNativeAncestors);
    m_vertical_tab_bar_column->setAttribute(Qt::WA_NativeWindow);
#endif
    m_vertical_tab_bar_column_layout = new QVBoxLayout(m_vertical_tab_bar_column);
    m_vertical_tab_bar_column->setProperty(VERTICAL_TABS_RESIZE_HANDLE_HOVERED_PROPERTY, false);
    m_vertical_tab_bar_column->setProperty(VERTICAL_TABS_RESIZE_HANDLE_ACTIVE_PROPERTY, false);
    m_vertical_tab_bar_column->installEventFilter(this);

    m_vertical_tabs_toggle_button = new VerticalTabsToggleButton(m_vertical_tab_bar_column);
    m_vertical_tabs_toggle_button->setObjectName("LadybirdVerticalTabsToggleButton");
    m_vertical_tabs_toggle_button->setDefaultAction(create_application_action(*this, Application::the().toggle_vertical_tabs_expanded_action()));
    m_vertical_tabs_toggle_button->setAutoRaise(true);
    m_vertical_tabs_toggle_button->setFocusPolicy(Qt::NoFocus);
    m_vertical_tabs_toggle_button->setIconSize({ 16, 16 });
    m_vertical_tabs_toggle_button->setFixedSize(VERTICAL_TABS_TOGGLE_BUTTON_SIZE, VERTICAL_TABS_TOGGLE_BUTTON_SIZE);

    m_vertical_tabs_content_separator = new QWidget(this);
    m_vertical_tabs_content_separator->setObjectName("LadybirdVerticalTabsContentSeparator");
    m_vertical_tabs_content_separator->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_vertical_tabs_content_separator->hide();

    m_vertical_tabs_reserved_space = new QWidget(m_vertical_tabs_content);
    m_vertical_tabs_reserved_space->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    // NB: While the sidebar narrows, the room it leaves shows the strip until the page takes it.
    m_vertical_tabs_reserved_space->setAutoFillBackground(true);

    m_vertical_tabs_resize_handle = new QWidget(this);
    m_vertical_tabs_resize_handle->setObjectName("LadybirdVerticalTabsResizeHandle");
#if defined(AK_OS_MACOS)
    m_vertical_tabs_resize_handle->setAttribute(Qt::WA_DontCreateNativeAncestors);
    m_vertical_tabs_resize_handle->setAttribute(Qt::WA_NativeWindow);
#endif
    m_vertical_tabs_resize_handle->setCursor(Qt::SizeHorCursor);
    m_vertical_tabs_resize_handle->installEventFilter(this);
    m_vertical_tabs_resize_handle->hide();

    m_vertical_tabs_hover_expand_timer = new QTimer(this);
    m_vertical_tabs_hover_expand_timer->setSingleShot(true);
    m_vertical_tabs_hover_expand_timer->setTimerType(Qt::PreciseTimer);
    connect(m_vertical_tabs_hover_expand_timer, &QTimer::timeout, this, [this]() {
        if (cursor_is_over_vertical_tabs())
            set_vertical_tabs_hover_expanded(true);
    });

    m_vertical_tabs_hover_expand_animation = new QVariantAnimation(this);
    m_vertical_tabs_hover_expand_animation->setEasingCurve(QEasingCurve::OutCubic);
    m_vertical_tabs_hover_expand_animation->setStartValue(0.0);
    m_vertical_tabs_hover_expand_animation->setEndValue(1.0);
    connect(m_vertical_tabs_hover_expand_animation, &QVariantAnimation::valueChanged, this, [this](QVariant const& value) {
        m_vertical_tabs_hover_expand_progress = value.toReal();
        update_tab_layout();
        update_vertical_tabs_content_overlay();
    });

    m_vertical_tabs_hover_collapse_timer = new QTimer(this);
    m_vertical_tabs_hover_collapse_timer->setInterval(VERTICAL_TABS_HOVER_COLLAPSE_POLL_INTERVAL_MS);
    connect(m_vertical_tabs_hover_collapse_timer, &QTimer::timeout, this, [this]() {
        update_vertical_tabs_hover_expanded();
    });

    m_main_layout = new QVBoxLayout(this);
    m_main_layout->setSpacing(0);
    m_main_layout->setContentsMargins(0, 0, 0, 0);
    rebuild_layout();
    update_chrome_style();

    connect(m_tab_bar, &QTabBar::currentChanged, this, [this](int index) {
        // The tab being left keeps its omnibox's editing session for when it is shown again.
        if (auto* previous_tab = qobject_cast<Tab*>(m_stacked_widget->currentWidget()); previous_tab && m_stacked_widget->indexOf(previous_tab) != index)
            previous_tab->park_location_editing();

        if (index >= 0 && index < m_stacked_widget->count())
            m_stacked_widget->setCurrentIndex(index);

        m_toolbar_container->setCurrentIndex(index);
        emit current_tab_changed(index);
        place_collapsed_sidebar_buttons();
        update_vertical_tabs_content_separator();
        update_vertical_tabs_overlay_geometry();
        update_vertical_tabs_resize_handle();
        update_vertical_tabs_content_overlay();
    });

    connect(m_tab_bar, &QTabBar::tabCloseRequested, this, &TabWidget::tab_close_requested);

    connect(m_tab_bar, &QTabBar::tabMoved, this, [this](int from, int to) {
        ScopeGuard guard { [&]() { m_stacked_widget->blockSignals(false); } };
        m_stacked_widget->blockSignals(true);

        auto* widget = m_stacked_widget->widget(from);
        m_stacked_widget->removeWidget(widget);
        m_stacked_widget->insertWidget(to, widget);

        auto* toolbar = as<Tab>(widget)->toolbar_container();
        if (auto toolbar_index = m_toolbar_container->indexOf(toolbar); toolbar_index != -1) {
            m_toolbar_container->removeWidget(toolbar);
            m_toolbar_container->insertWidget(to, toolbar);
        }
        m_stacked_widget->setCurrentIndex(m_tab_bar->currentIndex());
    });

    if constexpr (!use_native_macos_window_controls()) {
        connect(m_minimize_window_button, &QToolButton::clicked, this, [this] {
            window()->showMinimized();
        });
        connect(m_maximize_window_button, &QToolButton::clicked, this, [this] {
            toggle_window_maximized();
        });
        connect(m_close_window_button, &QToolButton::clicked, this, [this] {
            window()->close();
        });
    }
}

void TabWidget::add_tab(Tab* widget, QString const& label)
{
    insert_tab(m_tab_bar->count(), widget, label);
}

// Sizing the hidden page's view alone doesn't hold: The page itself still has its placeholder geometry, and the next
// pass of the page's own layout pulls the view back to it. So, give the whole page the current page's geometry, laid
// out now — and only then push the view's viewport.
// NB: A page that already had a real size is left alone. Sizing it would have it lay out anew, and wake its WebContent,
//     at every step of a window resize, for a size it would never show. It takes the window's size when it is shown.
static void size_page_like(Tab& page, Tab const& current)
{
    if (page.has_had_real_size())
        return;
    size_hidden_page_like(page, current);
    page.view().push_viewport_size();

    // NB: The current view takes several sizes in one turn of the event loop as the chrome around it settles, so the
    //     page has its real size only once the turn is over.
    if (current.isVisible()) {
        Core::deferred_invoke([page = QPointer<Tab>(&page)] {
            if (page)
                page->did_get_real_size();
        });
    }
}

void TabWidget::insert_tab(int index, Tab* widget, QString const& label)
{
    if (widget->is_loaded()) {
        // A page inserted behind the current one stays at Qt's default geometry until it's selected, and a load can
        // start into it long before that. Size its view like the current page's view first, so the page's first layout
        // sees the real viewport.
        if (auto* current = as_if<Tab>(m_stacked_widget->currentWidget()); current && current != widget && current->is_loaded())
            size_page_like(*widget, *current);
        widget->view().installEventFilter(this);
        widget->bookmarks_bar().installEventFilter(this);
    } else {
        // A tab that hasn't loaded gets the filters for its view and bookmarks bar once it has them, and the collapsed
        // sidebar's buttons once it has a toolbar to hold them.
        auto did_load = [this, widget] {
            widget->view().installEventFilter(this);
            widget->bookmarks_bar().installEventFilter(this);
            place_collapsed_sidebar_buttons();
        };
        connect(widget, &Tab::loaded, this, did_load, Qt::SingleShotConnection);
    }

    m_stacked_widget->insertWidget(index, widget);
    m_tab_bar->insertTab(index, label);
    widget->set_toolbar_container_in_tab_layout(false);
    m_toolbar_container->insertWidget(index, widget->toolbar_container());
    place_toolbar_over_page_column();

    widget->set_vertical_tabs_enabled(m_vertical_tabs_enabled);

    update_toolbar_placement();
    update_tab_layout();
    update_tab_button_visibility();

    // A tab moved here from another window can already be loading.
    m_tab_bar->update_loading_ring_animation();
}

void TabWidget::remove_tab(int index)
{
    take_tab(index);
}

Tab* TabWidget::take_tab(int index)
{
    auto* widget = m_stacked_widget->widget(index);
    if (!widget)
        return nullptr;

    // NB: Beside the collapsed sidebar, the Ladybird button and the sidebar's toggle sit in the current tab's toolbar,
    //     which the tab owns. They belong to this widget, so they don't go along with a tab that leaves it, or is
    //     deleted.
    auto* tab_toolbar = as<Tab>(widget)->toolbar_container();
    for (QWidget* button : { static_cast<QWidget*>(m_ladybird_button), static_cast<QWidget*>(m_vertical_tabs_toggle_button) }) {
        if (button && (widget->isAncestorOf(button) || tab_toolbar->isAncestorOf(button))) {
            button->hide();
            button->setParent(this);
        }
    }

    m_tab_bar->prepare_for_tab_removal(index);
    m_stacked_widget->removeWidget(widget);
    if (as<Tab>(widget)->is_loaded()) {
        as<Tab>(widget)->view().removeEventFilter(this);
        as<Tab>(widget)->bookmarks_bar().removeEventFilter(this);
    }
    disconnect(as<Tab>(widget), &Tab::loaded, this, nullptr);
    auto* toolbar = as<Tab>(widget)->toolbar_container();
    if (m_toolbar_container->indexOf(toolbar) != -1)
        m_toolbar_container->removeWidget(toolbar);
    as<Tab>(widget)->set_toolbar_container_in_tab_layout(true);

    m_tab_bar->removeTab(index);

    if (m_tab_bar->count() > 0 && m_tab_bar->currentIndex() >= 0)
        m_stacked_widget->setCurrentIndex(m_tab_bar->currentIndex());

    update_tab_layout();
    update_tab_button_visibility();
    place_collapsed_sidebar_buttons();
    return as<Tab>(widget);
}

void TabWidget::set_current_tab(Tab* widget)
{
    if (auto index = m_stacked_widget->indexOf(widget); index >= 0)
        set_current_index(index);
}

int TabWidget::index_of(Tab* widget) const
{
    return m_stacked_widget->indexOf(widget);
}

void TabWidget::set_new_tab_action(QAction* action)
{
    disconnect(m_new_tab_button, &QToolButton::clicked, nullptr, nullptr);

    if (!action)
        return;

    connect(m_new_tab_button, &QToolButton::clicked, action, &QAction::trigger);
}

void TabWidget::set_tab_bar_visible(bool visible)
{
    if (m_tab_bar_visible == visible)
        return;

    m_tab_bar_visible = visible;

    if (!m_tab_bar_visible)
        set_vertical_tabs_hover_expanded(false);

    update_tab_chrome_visibility();
    update_tab_layout();
}

void TabWidget::set_window_controls_visible(bool visible)
{
    m_window_controls_visible = visible;
    update_tab_toolbar_window_controls_visibility();
    update_tab_chrome_visibility();
    update_tab_layout();
}

void TabWidget::set_vertical_tabs_enabled(bool enabled)
{
    if (m_vertical_tabs_enabled == enabled)
        return;

    m_vertical_tabs_enabled = enabled;
    if (!m_vertical_tabs_enabled)
        set_vertical_tabs_hover_expanded(false);
    for (int index = 0; index < m_stacked_widget->count(); ++index) {
        tab(index)->set_vertical_tabs_enabled(enabled);
    }
    rebuild_layout();
}

void TabWidget::set_vertical_tabs_expanded(bool expanded)
{
    if (m_vertical_tabs_collapse_pending && !expanded)
        return;
    if (m_vertical_tabs_expanded == expanded && !m_vertical_tabs_collapse_pending)
        return;

    auto animate = m_vertical_tabs_enabled && m_tab_bar_visible && isVisible() && !SystemAppearance::prefers_reduced_motion();
    if (m_vertical_tabs_toggle_animation)
        m_vertical_tabs_toggle_animation->stop();
    m_vertical_tabs_toggling = false;
    m_vertical_tabs_collapse_pending = false;

    if (!animate) {
        m_vertical_tabs_expanded = expanded;
        set_vertical_tabs_hover_expanded(false);
        rebuild_layout();
        float_sidebar_buttons_while_toggling();
        slide_page_with_toggling_sidebar();
        return;
    }

    // The sidebar keeps its open layout while it narrows, and switches to the collapsed one at the end. Opening, it
    // switches at once and widens over the page.
    m_vertical_tabs_toggle_from_width = current_vertical_tabs_width();
    m_vertical_tabs_toggle_layout_width = vertical_tabs_layout_width();
    m_vertical_tabs_toggle_to_width = expanded ? m_vertical_tabs_expanded_width : VERTICAL_TABS_COLLAPSED_WIDTH;
    set_vertical_tabs_hover_expanded(false);
    if (expanded)
        m_vertical_tabs_expanded = true;
    else
        m_vertical_tabs_collapse_pending = true;
    m_vertical_tabs_toggling = true;
    m_vertical_tabs_toggle_progress = 0.0;
    // NB: The pictures of the buttons are taken where they are, before the layout changes.
    float_sidebar_buttons_while_toggling();
    rebuild_layout();
    float_sidebar_buttons_while_toggling();

    if (!m_vertical_tabs_toggle_animation) {
        m_vertical_tabs_toggle_animation = new QVariantAnimation(this);
        m_vertical_tabs_toggle_animation->setDuration(Motion::SLOW);
        m_vertical_tabs_toggle_animation->setEasingCurve(Motion::move_easing());
        m_vertical_tabs_toggle_animation->setStartValue(0.0);
        m_vertical_tabs_toggle_animation->setEndValue(1.0);
        connect(m_vertical_tabs_toggle_animation, &QVariantAnimation::valueChanged, this, [this](QVariant const& value) {
            m_vertical_tabs_toggle_progress = value.toReal();
            m_vertical_tab_bar_column->update();
            update_tab_layout();
            // NB: The pinned tiles move with the toggle, even on a step that leaves the tabs' width as it was.
            m_tab_bar->refresh_tab_layout();
            float_sidebar_buttons_while_toggling();
            slide_page_with_toggling_sidebar();
            update_vertical_tabs_content_overlay();
        });
        connect(m_vertical_tabs_toggle_animation, &QVariantAnimation::finished, this, [this] {
            m_vertical_tabs_toggling = false;
            if (m_vertical_tabs_collapse_pending) {
                m_vertical_tabs_collapse_pending = false;
                m_vertical_tabs_expanded = false;
            }
            rebuild_layout();
            float_sidebar_buttons_while_toggling();
            slide_page_with_toggling_sidebar();
        });
    }
    m_vertical_tabs_toggle_animation->start();
}

// The Ladybird button and the sidebar's toggle sit after the window controls in the toolbar beside the collapsed
// sidebar, and at the edge of the open sidebar's top row. While the sidebar opens or closes, neither layout holds them,
// so pictures of them stand in: The toggle rides the sidebar's edge, and pushes the Ladybird button along once it
// reaches it.
void TabWidget::float_sidebar_buttons_while_toggling()
{
    auto* current = as_if<Tab>(m_stacked_widget->currentWidget());
    auto floats = m_vertical_tabs_toggling && !vertical_tabs_are_on_right() && current && current->is_loaded();
    if (!floats) {
        for (auto* image : { m_floating_ladybird_button, m_floating_sidebar_toggle }) {
            if (image)
                image->hide();
        }
        return;
    }

    auto make_image = [this](QLabel*& image, QWidget& button) {
        if (!image) {
            image = new QLabel(this);
            image->setAttribute(Qt::WA_TransparentForMouseEvents);
        }
        // NB: The pictures are taken once, where the buttons were in place. A toggle that is reversed midway keeps them.
        if (!image->isVisible() && button.isVisible()) {
            auto device_pixel_ratio = button.devicePixelRatioF();
            QPixmap picture(button.size() * device_pixel_ratio);
            picture.setDevicePixelRatio(device_pixel_ratio);
            picture.fill(Qt::transparent);
            button.render(&picture, QPoint(), QRegion(), QWidget::DrawChildren);
            image->setPixmap(picture);
            image->setFixedSize(button.size());
        }
        button.hide();
        image->show();
        image->raise();
    };
    make_image(m_floating_ladybird_button, *m_ladybird_button);
    make_image(m_floating_sidebar_toggle, *m_vertical_tabs_toggle_button);

    auto column_origin = m_vertical_tab_bar_column->mapTo(this, QPoint());
    auto toolbar_height = Tab::toolbar_height(should_show_window_controls_in_tab_toolbar());
    auto center_y = [&](QWidget const& image) { return column_origin.y() + ((toolbar_height - image.height()) / 2); };

    auto& ladybird_button = *m_floating_ladybird_button;
    auto& toggle = *m_floating_sidebar_toggle;
    auto collapsed_ladybird_x = column_origin.x() + (vertical_tabs_hold_window_controls() ? NATIVE_MACOS_WINDOW_CONTROLS_INSET + NATIVE_MACOS_WINDOW_CONTROLS_WIDTH + LADYBIRD_BUTTON_TRAFFIC_LIGHTS_GAP : vertical_tabs_side_margin(true));
    auto collapsed_toggle_x = collapsed_ladybird_x + ladybird_button.width() + current->toolbar_sidebar_slot().layout()->spacing();
    auto at_edge_x = column_origin.x() + current_vertical_tabs_width() - vertical_tabs_side_margin(true) - SIDEBAR_TOGGLE_PAGE_SIDE_MARGIN - toggle.width();
    auto toggle_x = max(collapsed_toggle_x, at_edge_x);
    toggle.move(toggle_x, center_y(toggle));
    ladybird_button.move(max(collapsed_ladybird_x, toggle_x - SIDEBAR_LADYBIRD_BUTTON_GAP - ladybird_button.width()), center_y(ladybird_button));
}

// While the sidebar on the left opens or closes, the page and its toolbar slide along with the sidebar's edge, so neither
// is covered by the sidebar nor left apart from it. The page keeps the width it has beside the collapsed sidebar,
// reaching past the window's edge where the sidebar is wider, so it resizes only once: as the sidebar starts to close, or
// once it is open.
// NB: The sidebar on the right widens over the page instead, as sliding the toolbar to the left would take the window
//     controls and the navigation buttons under the window's edge.
bool TabWidget::slides_page_with_toggling_sidebar() const
{
    return m_vertical_tabs_toggling && !vertical_tabs_are_on_right();
}

void TabWidget::slide_page_with_toggling_sidebar()
{
    auto* current = as_if<Tab>(m_stacked_widget->currentWidget());
    if (!slides_page_with_toggling_sidebar()) {
        if (current)
            current->set_toolbar_leading_room(0);
        return;
    }
    // NB: The toolbar's contents start where they sit beside the collapsed sidebar, until the sidebar's edge reaches them.
    if (current)
        current->set_toolbar_leading_room(max(0, current->toolbar_lead_beside_collapsed_sidebar(*m_ladybird_button, COLLAPSED_VERTICAL_TABS_TOGGLE_BUTTON_SIZE) - current_vertical_tabs_width()));
    auto page_width = m_vertical_tabs_content->width() - min(m_vertical_tabs_toggle_from_width, m_vertical_tabs_toggle_to_width);
    m_page_column->setGeometry(current_vertical_tabs_width(), 0, page_width, m_vertical_tabs_content->height());
}

// While the sidebar collapses, its tab titles fade out at the start of its narrowing.
qreal TabWidget::vertical_tab_title_opacity() const
{
    if (!m_vertical_tabs_collapse_pending || !m_vertical_tabs_toggle_animation || m_vertical_tabs_toggle_animation->state() != QAbstractAnimation::Running)
        return 1;
    return max(0.0, 1 - (static_cast<qreal>(m_vertical_tabs_toggle_animation->currentTime()) / Motion::EXIT));
}

void TabWidget::set_vertical_tabs_expand_on_hover(bool expand_on_hover)
{
    if (m_vertical_tabs_expand_on_hover == expand_on_hover)
        return;

    m_vertical_tabs_expand_on_hover = expand_on_hover;
    if (!m_vertical_tabs_expand_on_hover)
        set_vertical_tabs_hover_expanded(false);
    rebuild_layout();
}

void TabWidget::set_vertical_tabs_position(WebView::VerticalTabsPosition position)
{
    if (m_vertical_tabs_position == position)
        return;

    m_vertical_tabs_position = position;
    set_vertical_tabs_hover_expanded(false);
    rebuild_layout();
}

bool TabWidget::event(QEvent* event)
{
    if (auto type = event->type(); type == QEvent::PaletteChange) {
        recreate_icons();
        update_chrome_style();
    } else if (type == QEvent::WindowStateChange) {
        update_window_button_icons();
    } else if (type == QEvent::WindowActivate || type == QEvent::WindowDeactivate) {
        // An inactive window's tab titles and strip icons drop to the muted color.
        recreate_icons();
        m_tab_bar->update();
        m_ladybird_button->update();
    } else if (type == QEvent::Leave) {
        defer_update_vertical_tabs_hover_expanded();
    }

    return QWidget::event(event);
}

void TabWidget::dragEnterEvent(QDragEnterEvent* event)
{
    accept_tab_drag(event, this);
}

void TabWidget::dragLeaveEvent(QDragLeaveEvent* event)
{
    m_tab_bar->hide_drop_preview();
    QWidget::dragLeaveEvent(event);
}

void TabWidget::dragMoveEvent(QDragMoveEvent* event)
{
    accept_tab_drag(event, this);
}

// A tab dropped elsewhere in the tab strip's area, such as past the new tab button, lands at the nearest end of the
// strip.
void TabWidget::dropEvent(QDropEvent* event)
{
    accept_tab_drop(event, m_tab_bar->drop_index_at(m_tab_bar->mapFrom(this, event->position().toPoint())));
}

bool TabWidget::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_tab_bar && (event->type() == QEvent::Move || event->type() == QEvent::Resize))
        place_horizontal_new_tab_button();

    if (watched == m_vertical_tab_bar_column && event->type() == QEvent::Paint) {
        paint_vertical_tabs_column();
        return true;
    }

    if (watched == m_tab_bar_row || watched == m_vertical_tab_bar_column) {
        auto* tab_strip = static_cast<QWidget*>(watched);

        switch (event->type()) {
        case QEvent::DragEnter:
        case QEvent::DragMove:
            accept_tab_drag(static_cast<QDragMoveEvent*>(event), tab_strip);
            return true;
        case QEvent::DragLeave:
            m_tab_bar->hide_drop_preview();
            return true;
        case QEvent::Drop: {
            auto* drop_event = static_cast<QDropEvent*>(event);
            accept_tab_drop(drop_event, m_tab_bar->drop_index_at(m_tab_bar->mapFrom(tab_strip, drop_event->position().toPoint())));
            return true;
        }
        default:
            break;
        }
    }

    if (is<BookmarksBar>(watched) && (event->type() == QEvent::Show || event->type() == QEvent::Hide))
        update_vertical_tabs_content_separator();

    if (watched == window() && event->type() == QEvent::Leave)
        defer_update_vertical_tabs_hover_expanded();

    if (event->type() == QEvent::Resize) {
        if (auto* current = as_if<Tab>(m_stacked_widget->currentWidget()); current && current->is_loaded() && watched == &current->view())
            size_hidden_pages_like_the_current_one();
    }

    if (watched == m_vertical_tabs_resize_handle) {
        auto reset_resize_handle = [this] {
            m_is_resizing_vertical_tabs = false;
            m_vertical_tabs_resize_handle->releaseMouse();
            QApplication::restoreOverrideCursor();
            set_resize_handle_property(VERTICAL_TABS_RESIZE_HANDLE_ACTIVE_PROPERTY, false);
        };

        if (event->type() == QEvent::Enter) {
            set_resize_handle_property(VERTICAL_TABS_RESIZE_HANDLE_HOVERED_PROPERTY, true);
        } else if (event->type() == QEvent::Leave) {
            if (!m_is_resizing_vertical_tabs)
                set_resize_handle_property(VERTICAL_TABS_RESIZE_HANDLE_HOVERED_PROPERTY, false);
        } else if (event->type() == QEvent::MouseButtonDblClick) {
            auto* mouse_event = static_cast<QMouseEvent*>(event);
            if (mouse_event->button() == Qt::LeftButton && m_vertical_tabs_expanded) {
                apply_vertical_tabs_expanded_width(VERTICAL_TABS_DEFAULT_EXPANDED_WIDTH);
                persist_vertical_tabs_expanded_width();
                return true;
            }
        } else if (event->type() == QEvent::MouseButtonPress) {
            auto* mouse_event = static_cast<QMouseEvent*>(event);
            if (mouse_event->button() != Qt::LeftButton)
                return QWidget::eventFilter(watched, event);

            m_is_resizing_vertical_tabs = true;
            m_vertical_tabs_resize_start_global_x = mouse_event->globalPosition().toPoint().x();
            m_vertical_tabs_resize_start_width = m_vertical_tabs_expanded_width;
            m_vertical_tabs_resize_handle->grabMouse();
            QApplication::setOverrideCursor(Qt::SizeHorCursor);
            set_resize_handle_property(VERTICAL_TABS_RESIZE_HANDLE_ACTIVE_PROPERTY, true);
            return true;
        } else if (event->type() == QEvent::MouseMove) {
            if (!m_is_resizing_vertical_tabs)
                return QWidget::eventFilter(watched, event);

            auto* mouse_event = static_cast<QMouseEvent*>(event);
            auto delta = mouse_event->globalPosition().toPoint().x() - m_vertical_tabs_resize_start_global_x;
            if (vertical_tabs_are_on_right())
                delta = -delta;
            apply_vertical_tabs_expanded_width(m_vertical_tabs_resize_start_width + delta);
            return true;
        } else if (event->type() == QEvent::MouseButtonRelease) {
            if (!m_is_resizing_vertical_tabs)
                return QWidget::eventFilter(watched, event);

            auto* mouse_event = static_cast<QMouseEvent*>(event);
            if (mouse_event->button() != Qt::LeftButton)
                return QWidget::eventFilter(watched, event);

            persist_vertical_tabs_expanded_width();
            reset_resize_handle();
            set_resize_handle_property(VERTICAL_TABS_RESIZE_HANDLE_HOVERED_PROPERTY, m_vertical_tabs_resize_handle->underMouse());
            return true;
        }
    }

    if (watched == m_vertical_tabs_content && event->type() == QEvent::Leave)
        defer_update_vertical_tabs_hover_expanded();

    if ((watched == m_page_column || watched == m_vertical_tabs_content) && (event->type() == QEvent::Move || event->type() == QEvent::Resize))
        place_toolbar_over_page_column();

    if (watched == m_toolbar_container && event->type() == QEvent::Resize) {
        update_vertical_tabs_column_margins();
        update_tab_layout();
    }

    auto is_vertical_tabs_hover_target = watched == m_vertical_tab_bar_column
        || watched == m_tab_bar
        || watched == m_new_tab_button;

    if (is_vertical_tabs_hover_target) {
        if (event->type() == QEvent::Enter) {
            if (can_expand_vertical_tabs_on_hover() && !m_vertical_tabs_hover_expanded && !m_vertical_tabs_hover_expand_timer->isActive()) {
                auto delay = Application::settings().config_variable_as_integer<int>(WebView::ConfigVariableID::VerticalTabsHoverExpandDelay);
                m_vertical_tabs_hover_expand_timer->start(delay);
            }
        } else if (event->type() == QEvent::Leave) {
            defer_update_vertical_tabs_hover_expanded();
        }
    }

    if ((watched == m_tab_bar_row || watched == m_vertical_tab_bar_column) && window_has_chrome_in_titlebar(*this)) {
        auto is_empty_chrome_area = [this, watched](QPoint position) {
            if (watched == m_vertical_tab_bar_column) {
                auto* child = m_vertical_tab_bar_column->childAt(position);
                return child == nullptr;
            }

            return m_tab_bar_row->childAt(position) == nullptr;
        };

        if (event->type() == QEvent::MouseButtonDblClick) {
            auto* mouse_event = static_cast<QMouseEvent*>(event);
            if (mouse_event->button() == Qt::LeftButton && is_empty_chrome_area(mouse_event->pos())) {
                toggle_window_maximized();
                return true;
            }
        }

        if (event->type() == QEvent::MouseButtonPress) {
            auto* mouse_event = static_cast<QMouseEvent*>(event);
            if (mouse_event->button() == Qt::LeftButton && is_empty_chrome_area(mouse_event->pos()) && start_window_move())
                return true;
        }

        if (event->type() == QEvent::ContextMenu) {
            auto* context_menu_event = static_cast<QContextMenuEvent*>(event);
            if (is_empty_chrome_area(context_menu_event->pos())) {
                show_window_context_menu(*this, context_menu_event->globalPos());
                return true;
            }
        }
    }

    return QWidget::eventFilter(watched, event);
}

void TabWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    update_tab_layout();
    slide_page_with_toggling_sidebar();
}

// The stacked layout sizes only the current page, so the tabs behind it keep stale geometry: Qt's default 100x30 for
// a tab that was created before the window was first shown — which is every tab but the first on a command line, and
// every restored one. Their pages may already be loading. So, whenever the current page's view gets its size, then size
// every hidden page's view the same, until it has had a real size. A tab that hasn't loaded is skipped: It has no view
// yet, and it loads only once it's the current page, which the stacked layout sizes.
// NB: This hangs off the view's own resize event, not this widget's: At the window's first show, this widget is laid
// out before the current page's child layout has run. So at that point the current view still has its placeholder size.
void TabWidget::size_hidden_pages_like_the_current_one()
{
    auto* current = as_if<Tab>(m_stacked_widget->currentWidget());
    if (!current || !current->is_loaded())
        return;
    for (int i = 0; i < m_stacked_widget->count(); ++i) {
        if (auto* page = as_if<Tab>(m_stacked_widget->widget(i)); page && page != current && page->is_loaded())
            size_page_like(*page, *current);
    }
}

void TabWidget::accept_tab_drag(QDragMoveEvent* event, QWidget* target)
{
    if (!target_can_accept_tab_drop(*this, *event)) {
        m_tab_bar->hide_drop_preview();
        event->ignore();
        return;
    }

    if (!tab_drop_area_contains(*target, event->position().toPoint())) {
        m_tab_bar->hide_drop_preview();
        event->ignore();
        return;
    }

    m_tab_bar->update_drop_preview(m_tab_bar->mapFrom(target, event->position().toPoint()));
    event->setDropAction(Qt::MoveAction);
    event->accept();
}

void TabWidget::accept_tab_drop(QDropEvent* event, int index)
{
    if (!target_can_accept_tab_drop(*this, *event)) {
        event->ignore();
        return;
    }

    s_pending_tab_drop_target = this;
    s_pending_tab_drop_index = index;
    event->setDropAction(Qt::MoveAction);
    event->accept();
}

TabLayout TabWidget::current_tab_layout() const
{
    if (!m_vertical_tabs_enabled)
        return TabLayout::Horizontal;
    return vertical_tabs_effectively_expanded() ? TabLayout::VerticalExpanded : TabLayout::VerticalCollapsed;
}

// A tab dragged a little way off the strip, onto the toolbar for example, still lands in it. Only one dragged further
// away is detached into a window of its own.
bool TabWidget::tab_drop_area_contains(QWidget const& widget, QPoint position) const
{
    auto* drag_area = tab_drag_area_widget();
    auto position_in_drag_area = drag_area->mapFrom(window(), widget.mapTo(window(), position));
    auto drop_area = m_tab_bar->tab_layout() == TabLayout::Horizontal
        ? drag_area->rect().adjusted(0, -TAB_DETACH_DISTANCE, 0, TAB_DETACH_DISTANCE)
        : drag_area->rect().adjusted(-TAB_DETACH_DISTANCE, 0, TAB_DETACH_DISTANCE, 0);
    return drop_area.contains(position_in_drag_area);
}

QWidget* TabWidget::tab_drag_area_widget() const
{
    return m_tab_bar->tab_layout() == TabLayout::Horizontal ? m_tab_bar_row : m_vertical_tab_bar_column;
}

bool TabWidget::vertical_tabs_effectively_expanded() const
{
    return m_vertical_tabs_expanded || m_vertical_tabs_hover_expanded;
}

bool TabWidget::can_expand_vertical_tabs_on_hover() const
{
    return m_tab_bar_visible && m_vertical_tabs_enabled && m_vertical_tabs_expand_on_hover && !m_vertical_tabs_expanded;
}

bool TabWidget::cursor_is_over_vertical_tabs() const
{
    if (!m_vertical_tabs_content->isVisible())
        return false;

    if (m_vertical_tab_bar_column->underMouse() || m_tab_bar->underMouse() || m_new_tab_button->underMouse())
        return true;

    auto vertical_tabs_x = vertical_tabs_are_on_right() ? max(0, m_vertical_tabs_content->width() - current_vertical_tabs_width()) : 0;
    auto vertical_tabs_rect = QRect {
        m_vertical_tabs_content->mapToGlobal(QPoint { vertical_tabs_x, 0 }),
        QSize { current_vertical_tabs_width(), m_vertical_tabs_content->height() },
    };
    return window()->underMouse() && vertical_tabs_rect.contains(QCursor::pos());
}

bool TabWidget::vertical_tabs_are_on_right() const
{
    return m_vertical_tabs_position == WebView::VerticalTabsPosition::Right;
}

int TabWidget::vertical_tabs_layout_width() const
{
    if (!m_tab_bar_visible)
        return 0;
    // NB: While the sidebar opens or closes, the page keeps its room, and resizes once at the end.
    if (m_vertical_tabs_toggling)
        return m_vertical_tabs_toggle_layout_width;
    return m_vertical_tabs_expanded ? m_vertical_tabs_expanded_width : VERTICAL_TABS_COLLAPSED_WIDTH;
}

bool TabWidget::should_show_window_controls_in_tab_toolbar() const
{
    return m_window_controls_visible && m_tab_bar->tab_layout() != TabLayout::Horizontal && !vertical_tabs_hold_window_controls();
}

// The native window controls sit in the window's top left corner, so the open sidebar on the left holds them.
bool TabWidget::vertical_tabs_hold_window_controls() const
{
    return use_native_macos_window_controls() && m_window_controls_visible && m_vertical_tabs_expanded && !vertical_tabs_are_on_right();
}

// The open sidebar's top row is level with the toolbar, and the collapsed sidebar starts below the toolbar and the line
// under it. The open sidebar's tabs start as far down as the collapsed sidebar's, so they stay in place as it toggles.
int TabWidget::vertical_tabs_header_spacing() const
{
    auto toolbar_line_height = max(0, m_toolbar_container->height() - Tab::toolbar_height(should_show_window_controls_in_tab_toolbar()));
    return VERTICAL_TABS_TOP_MARGIN - VERTICAL_TAB_BAR_END_ROOM + toolbar_line_height;
}

// On macOS, the open sidebar is at its narrowest when its Ladybird button and toggle sit where they do beside the
// collapsed sidebar, so neither moves as it toggles.
int TabWidget::minimum_vertical_tabs_expanded_width() const
{
    if (!use_native_macos_window_controls() || !m_ladybird_button)
        return VERTICAL_TABS_MIN_EXPANDED_WIDTH;
    return NATIVE_MACOS_WINDOW_CONTROLS_INSET + NATIVE_MACOS_WINDOW_CONTROLS_WIDTH + LADYBIRD_BUTTON_TRAFFIC_LIGHTS_GAP
        + m_ladybird_button->width() + SIDEBAR_LADYBIRD_BUTTON_GAP + VERTICAL_TABS_TOGGLE_BUTTON_SIZE
        + SIDEBAR_TOGGLE_PAGE_SIDE_MARGIN + vertical_tabs_side_margin(true);
}

// How far the opening or closing sidebar is from open, from 0 when open to 1 when collapsed.
qreal TabWidget::vertical_tabs_toggle_collapsedness() const
{
    return m_vertical_tabs_collapse_pending ? m_vertical_tabs_toggle_progress : 1 - m_vertical_tabs_toggle_progress;
}

void TabWidget::update_toolbar_placement()
{
    m_toolbar_container->setCurrentIndex(m_tab_bar->currentIndex());
    update_tab_toolbar_window_controls_visibility();
}

void TabWidget::update_tab_toolbar_window_controls_visibility()
{
    auto show_controls = should_show_window_controls_in_tab_toolbar();
    for (int index = 0; index < m_stacked_widget->count(); ++index)
        tab(index)->set_toolbar_window_controls_visible(show_controls);
}

void TabWidget::rebuild_layout()
{
    // NB: Moving the page stack between layouts drops keyboard focus from the page, which leaves the window's keyboard
    //     shortcuts dead until something is clicked. Give focus back to whatever had it.
    QPointer<QWidget> focus_widget = QApplication::focusWidget();
    if (focus_widget && !isAncestorOf(focus_widget))
        focus_widget = nullptr;
    ScopeGuard restore_focus = [&] {
        if (!focus_widget)
            return;
        auto restore = [focus_widget = focus_widget] {
            if (!focus_widget || !focus_widget->isVisible())
                return;
#if defined(AK_OS_MACOS)
            make_appkit_window_first_responder(*focus_widget);
#endif
            focus_widget->setFocus();
        };
        // NB: A widget that moved under an already visible parent, like the toolbar does when the tabs move to the
        //     side, is only shown again on the next turn of the event loop.
        if (focus_widget->isVisible())
            restore();
        else
            QTimer::singleShot(0, focus_widget.data(), restore);
    };

    clear_layout(*m_main_layout);
    clear_layout(*m_tab_bar_row_layout);
    clear_layout(*m_page_column_layout);
    clear_layout(*m_vertical_tab_bar_column_layout);
    m_vertical_tabs_header_spacer = nullptr;
    clear_layout(*m_vertical_tabs_content_layout);

    m_tab_bar->set_vertical_tabs_position(m_vertical_tabs_position);
    m_tab_bar->set_pinned_tabs_in_rows(m_vertical_tabs_hover_expanded && !m_vertical_tabs_expanded);
    m_tab_bar->set_tab_layout(current_tab_layout());
    update_toolbar_placement();

    if (m_tab_bar->tab_layout() != TabLayout::Horizontal) {
        rebuild_layout_for_vertical_tabs();

        // The open sidebar runs the full height of the window, with the toolbar beside it above the page. The collapsed
        // sidebar sits below a toolbar across the whole window.
        // NB: The page stays in the page column either way, as a page that moves to another parent is shown at a stale
        //     place for a few frames on macOS.
        QWidget* page = m_page_column;
        if (m_vertical_tabs_expanded) {
            rebuild_page_column();
        } else {
            m_toolbar_follows_page_column = false;
            m_toolbar_placeholder->hide();
            m_main_layout->addWidget(m_toolbar_container);
            m_page_column_layout->addWidget(m_stacked_widget, 1);
        }
        m_page_column->show();

        if (slides_page_with_toggling_sidebar()) {
            // NB: While the sidebar opens or closes, the page is placed by slide_page_with_toggling_sidebar() rather
            //     than laid out.
            if (page->parentWidget() != m_vertical_tabs_content)
                page->setParent(m_vertical_tabs_content);
            m_vertical_tabs_reserved_space->hide();
            page->show();
        } else if (vertical_tabs_are_on_right()) {
            m_vertical_tabs_content_layout->addWidget(page, 1);
            m_vertical_tabs_content_layout->addWidget(m_vertical_tabs_reserved_space);
            m_vertical_tabs_reserved_space->show();
        } else {
            m_vertical_tabs_content_layout->addWidget(m_vertical_tabs_reserved_space);
            m_vertical_tabs_content_layout->addWidget(page, 1);
            m_vertical_tabs_reserved_space->show();
        }
        m_main_layout->addWidget(m_vertical_tabs_content, 1);
        m_vertical_tabs_content->show();
        if (slides_page_with_toggling_sidebar()) {
            m_main_layout->activate();
            slide_page_with_toggling_sidebar();
        }
    } else {
        rebuild_layout_for_horizontal_tabs();
        rebuild_page_column();

        m_main_layout->addWidget(m_tab_bar_row);
        m_main_layout->addWidget(m_page_column, 1);
        m_page_column->show();
        m_vertical_tabs_content->hide();
    }

    update_tab_button_visibility();
    update_tab_chrome_visibility();
    update_tab_layout();
    place_collapsed_sidebar_buttons();
    m_main_layout->activate();
    place_toolbar_over_page_column();
}

void TabWidget::rebuild_layout_for_horizontal_tabs()
{
    m_tab_bar_row->setMinimumHeight(HORIZONTAL_TAB_STRIP_HEIGHT);
    m_tab_bar_row_layout->setSpacing(0);
    m_tab_bar_row_layout->setContentsMargins(HORIZONTAL_TAB_STRIP_LEADING_MARGIN, HORIZONTAL_TAB_STRIP_HEIGHT - HORIZONTAL_TAB_HEIGHT, 8, 0);

    set_dynamic_property_if_needed(*m_new_tab_button, VERTICAL_TABS_EXPANDED_PROPERTY, false);
    set_dynamic_property_if_needed(*m_new_tab_button, VERTICAL_TABS_BUTTON_PROPERTY, true);
    m_new_tab_button->setFixedSize(HORIZONTAL_NEW_TAB_BUTTON_HIT_SIZE, HORIZONTAL_NEW_TAB_BUTTON_HIT_SIZE);
    m_new_tab_button->setIconSize(QSize(HORIZONTAL_NEW_TAB_BUTTON_ICON_SIZE, HORIZONTAL_NEW_TAB_BUTTON_ICON_SIZE));
    m_new_tab_button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    // NB: The tabs sit on the bottom of the strip, so the selected tab meets the toolbar however tall the strip is.
    // NB: The new tab button is placed by place_horizontal_new_tab_button(), as its gap is measured from the last tab
    //     rather than from that tab's room for its foot. The row leaves room for it after the tab bar.
    m_new_tab_button->setParent(m_tab_bar_row);
    m_new_tab_button->show();
    auto new_tab_button_room = HORIZONTAL_NEW_TAB_BUTTON_GAP - HORIZONTAL_TAB_END_ROOM + HORIZONTAL_NEW_TAB_BUTTON_SIZE + horizontal_tab_strip_drag_space();
    // The Ladybird button comes first after the traffic lights, and first on the strip where the window controls are
    // on the other side. Like the new tab button, it is centered on the middle of the tabs.
    // NB: The first tab's room for its foot takes part of the gap after the Ladybird button.
    m_ladybird_button->setParent(m_tab_bar_row);
    // NB: The button sits at the bottom of the tab row, and its tile is centered in the strip.
    m_ladybird_button->set_tile_vertical_offset(HORIZONTAL_WINDOW_ITEM_CENTER - (HORIZONTAL_TAB_STRIP_HEIGHT - HORIZONTAL_TAB_HEIGHT) - (m_ladybird_button->sizeHint().height() / 2));
    m_ladybird_button->show();
    auto add_ladybird_button = [&] {
        m_tab_bar_row_layout->addWidget(m_ladybird_button, 0, Qt::AlignBottom);
        m_tab_bar_row_layout->addSpacing(LADYBIRD_BUTTON_TRAILING_GAP - HORIZONTAL_TAB_END_ROOM);
    };
    if constexpr (use_native_macos_window_controls()) {
        m_tab_bar_row_layout->addWidget(m_window_controls, 0, Qt::AlignVCenter);
        m_tab_bar_row_layout->addSpacing(LADYBIRD_BUTTON_TRAFFIC_LIGHTS_GAP);
        add_ladybird_button();
        m_tab_bar_row_layout->addWidget(m_tab_bar, 0, Qt::AlignBottom);
        m_tab_bar_row_layout->addSpacing(new_tab_button_room);
        m_tab_bar_row_layout->addStretch(1);
    } else {
        add_ladybird_button();
        m_tab_bar_row_layout->addWidget(m_tab_bar, 0, Qt::AlignBottom);
        m_tab_bar_row_layout->addSpacing(new_tab_button_room);
        m_tab_bar_row_layout->addStretch(1);
        m_tab_bar_row_layout->addWidget(m_window_controls, 0, Qt::AlignTop);
    }
    place_horizontal_new_tab_button();
}

void TabWidget::place_horizontal_new_tab_button()
{
    if (m_tab_bar->tab_layout() != TabLayout::Horizontal)
        return;

    // The button is centered on the middle of the tabs, like everything else on the strip.
    auto tab_bar_geometry = m_tab_bar->geometry();
    // NB: While tabs open and close, the button follows the end of the tabs, rather than the end of the tab bar. The bar
    //     already grows to hold tabs that end beyond where they settle (see update_tab_layout()).
    auto x = tab_bar_geometry.right() + 1 - HORIZONTAL_TAB_END_ROOM + HORIZONTAL_NEW_TAB_BUTTON_GAP + min(0, m_tab_bar->horizontal_tabs_end_offset());
    // NB: The buttons are centered on the tabs' cards, which the strip's line runs through.
    auto card_height = HORIZONTAL_TAB_HEIGHT - HORIZONTAL_TAB_CARD_SHAPE_TOP_INSET - HORIZONTAL_TAB_CARD_SHAPE_BOTTOM_INSET;
    auto y = tab_bar_geometry.top() + HORIZONTAL_TAB_CARD_SHAPE_TOP_INSET + ((card_height - m_new_tab_button->height()) / 2);
    // NB: Before the window is first shown, no widget in it is visible, so this asks whether the button is hidden.
    if (!m_all_tabs_button->isHidden()) {
        m_all_tabs_button->move(x, y);
        m_all_tabs_button->raise();
        x += HORIZONTAL_NEW_TAB_BUTTON_SIZE;
    }
    m_new_tab_button->move(x + ((HORIZONTAL_NEW_TAB_BUTTON_SIZE - m_new_tab_button->width()) / 2), y);
    m_new_tab_button->raise();
}

// NB: Where the strip ends at the window's edge, its margin there is part of the free space.
int TabWidget::horizontal_tab_strip_drag_space() const
{
    if constexpr (use_native_macos_window_controls())
        return HORIZONTAL_TAB_STRIP_DRAG_SPACE - m_tab_bar_row_layout->contentsMargins().right();
    return HORIZONTAL_TAB_STRIP_DRAG_SPACE;
}

static constexpr size_t RECENTLY_CLOSED_LIMIT = 5;

static QString time_since_closed(AK::Duration duration)
{
    auto minutes = duration.to_seconds() / 60;
    if (minutes < 1)
        return QStringLiteral("Just now");
    if (minutes < 60)
        return qformatted("{} min ago", minutes);
    auto hours = minutes / 60;
    if (hours < 24)
        return hours == 1 ? QStringLiteral("1 hour ago") : qformatted("{} hours ago", hours);
    auto days = hours / 24;
    return days == 1 ? QStringLiteral("Yesterday") : qformatted("{} days ago", days);
}

void TabWidget::show_all_tabs()
{
    if (!m_all_tabs_popover) {
        m_all_tabs_popover = new AllTabsPopover(this);
        m_all_tabs_popover->update_chrome_style(palette());
        m_all_tabs_popover->on_tab_activated = [this](size_t index) {
            if (static_cast<int>(index) < count())
                set_current_index(static_cast<int>(index));
        };
        m_all_tabs_popover->on_recently_closed_activated = [this](size_t index) {
            if (index < m_recently_closed_unit_ids.size())
                Application::the().reopen_recently_closed_tab(as_if<BrowserWindow>(window()), m_recently_closed_unit_ids[index]);
        };
    }

    Vector<AllTabsPopover::Entry> entries;
    entries.ensure_capacity(count());
    for (int index = 0; index < count(); ++index) {
        auto* tab = this->tab(index);
        auto url = tab->url();
        entries.unchecked_append({
            .icon = tab->favicon(),
            .title = tab->title(),
            .detail = url.host().has_value() ? qstring_from_ak_string(url.serialized_host()) : QString {},
            .is_current = index == current_index(),
        });
    }
    m_all_tabs_popover->set_tabs(AK::move(entries));

    Vector<AllTabsPopover::Entry> closed_entries;
    m_recently_closed_unit_ids.clear();
    if (auto* browser_window = as_if<BrowserWindow>(window())) {
        auto& session = browser_window->session();
        auto now = UnixDateTime::now();
        for (auto const& unit : session.session_store->recently_closed_units(RECENTLY_CLOSED_LIMIT)) {
            auto entry = session.history_store->entry_for_url(unit.active_url);
            auto title = entry.has_value() && entry->title.has_value() && !entry->title->is_empty() ? qstring_from_ak_string(*entry->title) : qstring_from_ak_string(unit.active_url.serialize());
            if (unit.was_window)
                title = unit.tab_count == 1 ? QStringLiteral("Window with 1 tab") : qformatted("Window with {} tabs", unit.tab_count);
            closed_entries.append({
                .icon = entry.has_value() && entry->favicon_png.has_value() ? icon_from_png(entry->favicon_png->bytes(), TAB_ICON_SIZE) : QIcon {},
                .title = AK::move(title),
                .detail = time_since_closed(now - unit.closed_at),
                .is_current = false,
            });
            m_recently_closed_unit_ids.append(unit.id);
        }
    }
    m_all_tabs_popover->set_recently_closed(AK::move(closed_entries));

    // The list hangs from the button that opens it, or else from the Ladybird button.
    if (!m_all_tabs_button->isHidden())
        move_popover_below(*m_all_tabs_popover, *m_all_tabs_button, PopoverAlignment::Trailing);
    else
        move_popover_below(*m_all_tabs_popover, *m_ladybird_button, PopoverAlignment::Leading, m_ladybird_button->tile_rect());
    m_all_tabs_popover->show();
}

void TabWidget::place_collapsed_sidebar_buttons()
{
    auto collapsed = m_tab_bar_visible && m_tab_bar->tab_layout() != TabLayout::Horizontal && !m_vertical_tabs_expanded;
    auto* current = as_if<Tab>(m_stacked_widget->currentWidget());
    // A tab that hasn't loaded has no toolbar yet, so there's no slot of its own to hide or to fill.
    for (int index = 0; index < m_stacked_widget->count(); ++index) {
        if (auto* tab = this->tab(index); tab && tab->is_loaded() && (tab != current || !collapsed))
            tab->toolbar_sidebar_slot().hide();
    }
    if (!current || !current->is_loaded() || !collapsed)
        return;

    auto& slot = current->toolbar_sidebar_slot();
    auto* slot_layout = static_cast<QBoxLayout*>(slot.layout());
    if (m_ladybird_button->parentWidget() != &slot) {
        m_ladybird_button->setParent(&slot);
        m_ladybird_button->set_tile_vertical_offset(0);
        slot_layout->insertWidget(0, m_ladybird_button, 0, Qt::AlignVCenter);
    }
    if (m_vertical_tabs_toggle_button->parentWidget() != &slot) {
        m_vertical_tabs_toggle_button->setParent(&slot);
        m_vertical_tabs_toggle_button->setFixedSize(COLLAPSED_VERTICAL_TABS_TOGGLE_BUTTON_SIZE);
        slot_layout->insertWidget(1, m_vertical_tabs_toggle_button, 0, Qt::AlignVCenter);
    }
    m_ladybird_button->show();
    m_vertical_tabs_toggle_button->show();
    slot.show();
}

void TabWidget::rebuild_layout_for_vertical_tabs()
{
    update_vertical_tabs_toggle_icon();
    auto reserved_width = vertical_tabs_layout_width();
    auto side_bar_width = current_vertical_tabs_width();
    m_vertical_tabs_reserved_space->setFixedWidth(reserved_width);
    m_vertical_tab_bar_column->setFixedWidth(side_bar_width);
    set_dynamic_property_if_needed(*m_vertical_tab_bar_column, VERTICAL_TABS_POSITION_PROPERTY, vertical_tabs_position_property_value(m_vertical_tabs_position));

    m_vertical_tab_bar_column_layout->setSpacing(0);
    if (auto* window_controls_layout = m_window_controls->layout())
        window_controls_layout->setContentsMargins(0, 0, 0, 0);
    update_vertical_tabs_column_margins();

    update_vertical_tabs_button_layout();

    auto* top_row_layout = new QHBoxLayout;
    top_row_layout->setSpacing(0);
    if (m_vertical_tabs_expanded) {
        // The open sidebar runs the full height of the window. Its top row sits beside the toolbar, and holds the window
        // controls when they are on its side, with the Ladybird button and the sidebar's toggle at the edge toward the
        // page.
        top_row_layout->setContentsMargins(vertical_tabs_are_on_right() ? SIDEBAR_TOGGLE_PAGE_SIDE_MARGIN : 0, 0, vertical_tabs_are_on_right() ? 0 : SIDEBAR_TOGGLE_PAGE_SIDE_MARGIN, 0);
        top_row_layout->addStrut(Tab::toolbar_height(should_show_window_controls_in_tab_toolbar()));
        m_vertical_tabs_toggle_button->setParent(m_vertical_tab_bar_column);
        // NB: The button was smaller while it sat in the collapsed sidebar's slot.
        m_vertical_tabs_toggle_button->setFixedSize(VERTICAL_TABS_TOGGLE_BUTTON_SIZE, VERTICAL_TABS_TOGGLE_BUTTON_SIZE);
        m_vertical_tabs_toggle_button->show();
        if (vertical_tabs_are_on_right()) {
            top_row_layout->addWidget(m_vertical_tabs_toggle_button, 0, Qt::AlignVCenter);
            top_row_layout->addSpacing(SIDEBAR_LADYBIRD_BUTTON_GAP);
            top_row_layout->addWidget(m_ladybird_button, 0, Qt::AlignVCenter);
            top_row_layout->addStretch(1);
        } else {
            top_row_layout->addStretch(1);
            top_row_layout->addWidget(m_ladybird_button, 0, Qt::AlignVCenter);
            top_row_layout->addSpacing(SIDEBAR_LADYBIRD_BUTTON_GAP);
            top_row_layout->addWidget(m_vertical_tabs_toggle_button, 0, Qt::AlignVCenter);
        }
    } else {
        // The collapsed rail holds only tabs. The Ladybird button and the sidebar's toggle move to the toolbar, after
        // the window controls (see place_collapsed_sidebar_buttons()). Its tabs start as far below the toolbar as the
        // open sidebar's start below its top row.
        m_vertical_tab_bar_column_layout->addSpacing(VERTICAL_TABS_TOP_MARGIN - VERTICAL_TAB_BAR_END_ROOM);
    }
    if (m_vertical_tabs_expanded) {
        m_ladybird_button->setParent(m_vertical_tab_bar_column);
        m_ladybird_button->set_tile_vertical_offset(0);
        m_ladybird_button->show();
        m_vertical_tab_bar_column_layout->addLayout(top_row_layout);
        m_vertical_tabs_header_spacer = new QSpacerItem(0, vertical_tabs_header_spacing(), QSizePolicy::Fixed, QSizePolicy::Fixed);
        m_vertical_tab_bar_column_layout->addItem(m_vertical_tabs_header_spacer);
    } else {
        delete top_row_layout;
    }

    m_vertical_tab_bar_column_layout->addWidget(m_tab_bar);
    m_vertical_tab_bar_column_layout->addWidget(m_new_tab_button, 0, vertical_tabs_are_on_right() ? Qt::AlignRight : Qt::AlignLeft);
    m_vertical_tab_bar_column_layout->addStretch(1);
}

// A divider runs along the sidebar's edge toward the page.
void TabWidget::paint_vertical_tabs_column()
{
    QPainter painter(m_vertical_tab_bar_column);
    QStyleOption option;
    option.initFrom(m_vertical_tab_bar_column);
    m_vertical_tab_bar_column->style()->drawPrimitive(QStyle::PE_Widget, &option, &painter, m_vertical_tab_bar_column);

    auto const& chrome = Theme::chrome(*this);
    auto column_rect = m_vertical_tab_bar_column->rect();
    auto page_edge_x = vertical_tabs_are_on_right() ? column_rect.left() : column_rect.right();

    // NB: While the sidebar opens or closes, its top row turns between its own color and the toolbar's, which the
    //     collapsed sidebar sits below, so that the corner doesn't change color at once as the layout switches.
    auto top_row_height = 0;
    if (m_vertical_tabs_toggling) {
        auto collapsedness = vertical_tabs_toggle_collapsedness();
        top_row_height = Tab::toolbar_height(should_show_window_controls_in_tab_toolbar());
        painter.fillRect(QRect(0, 0, column_rect.width(), top_row_height), ChromeStyle::mix(chrome.sidebar, chrome.toolbar, collapsedness));
        auto faded_divider = chrome.divider;
        faded_divider.setAlphaF(faded_divider.alphaF() * (1 - collapsedness));
        painter.fillRect(QRect(page_edge_x, 0, 1, top_row_height), faded_divider);
    }
    painter.fillRect(QRect(page_edge_x, top_row_height, 1, column_rect.height() - top_row_height), chrome.divider);
}

void TabWidget::update_vertical_tabs_column_margins()
{
    // NB: In the open sidebar, the tabs are inset alike from both of its sides. The collapsed sidebar's tab bar reaches
    //     the page's side of it.
    auto expanded = m_tab_bar->tab_layout() != TabLayout::VerticalCollapsed;
    auto side_margin = vertical_tabs_side_margin(expanded);
    auto page_side_margin = expanded ? side_margin : 0;
    auto left = vertical_tabs_are_on_right() ? page_side_margin : side_margin;
    auto right = vertical_tabs_are_on_right() ? side_margin : page_side_margin;
    m_vertical_tab_bar_column_layout->setContentsMargins(left, 0, right, 8);
    if (m_vertical_tabs_header_spacer && m_vertical_tabs_header_spacer->sizeHint().height() != vertical_tabs_header_spacing()) {
        m_vertical_tabs_header_spacer->changeSize(0, vertical_tabs_header_spacing(), QSizePolicy::Fixed, QSizePolicy::Fixed);
        m_vertical_tab_bar_column_layout->invalidate();
    }
}

// The toolbar heads the page column, except beside the collapsed sidebar, where it runs across the whole window.
// NB: The toolbar is never moved to another parent, as that would hide it for a frame and take focus from the address
//     field. Instead, it is placed over a stand-in at the top of the page column.
void TabWidget::rebuild_page_column()
{
    m_toolbar_follows_page_column = true;
    m_toolbar_placeholder->setVisible(m_tab_bar_visible);
    m_page_column_layout->addWidget(m_toolbar_placeholder);
    m_page_column_layout->addWidget(m_stacked_widget, 1);
}

void TabWidget::place_toolbar_over_page_column()
{
    if (!m_toolbar_follows_page_column)
        return;

    // NB: The stand-in is as tall as the toolbar, which is only known once a tab's toolbar is in it.
    if (auto toolbar_height = m_toolbar_container->sizeHint().height(); m_toolbar_placeholder->height() != toolbar_height) {
        m_toolbar_placeholder->setFixedHeight(toolbar_height);
        m_page_column_layout->activate();
    }

    auto position = m_page_column->mapTo(this, QPoint());
    m_toolbar_container->setGeometry(position.x(), position.y(), m_page_column->width(), m_toolbar_placeholder->height());
    m_toolbar_container->stackUnder(m_vertical_tab_bar_column);

    // Reparenting the page column when switching tab layouts can put its placeholder above the toolbar.
    if (m_page_column->parentWidget() == this)
        m_page_column->stackUnder(m_toolbar_container);

    if (m_vertical_tabs_content_separator->isVisible())
        m_vertical_tabs_content_separator->raise();
}

// The width of the vertical tabs where the opening or closing sidebar is open, and where it is collapsed.
int TabWidget::vertical_tabs_toggle_open_tab_width() const
{
    auto sidebar_width = m_vertical_tabs_collapse_pending ? m_vertical_tabs_toggle_from_width : m_vertical_tabs_toggle_to_width;
    return max(0, sidebar_width - vertical_tabs_horizontal_margin_width(m_tab_bar->tab_layout()));
}

int TabWidget::vertical_tabs_toggle_collapsed_tab_width() const
{
    auto sidebar_width = m_vertical_tabs_collapse_pending ? m_vertical_tabs_toggle_to_width : m_vertical_tabs_toggle_from_width;
    return max(0, sidebar_width - vertical_tabs_horizontal_margin_width(m_tab_bar->tab_layout()));
}

int TabWidget::current_vertical_tabs_width() const
{
    if (m_vertical_tabs_toggling)
        return m_vertical_tabs_toggle_from_width + qRound((m_vertical_tabs_toggle_to_width - m_vertical_tabs_toggle_from_width) * m_vertical_tabs_toggle_progress);
    if (m_vertical_tabs_hover_expanded)
        return VERTICAL_TABS_COLLAPSED_WIDTH + qRound((m_vertical_tabs_expanded_width - VERTICAL_TABS_COLLAPSED_WIDTH) * m_vertical_tabs_hover_expand_progress);

    return vertical_tabs_effectively_expanded() ? m_vertical_tabs_expanded_width : VERTICAL_TABS_COLLAPSED_WIDTH;
}

void TabWidget::apply_vertical_tabs_expanded_width(int width)
{
    auto clamped_width = clamp(width, minimum_vertical_tabs_expanded_width(), VERTICAL_TABS_MAX_EXPANDED_WIDTH);
    if (m_vertical_tabs_expanded_width == clamped_width)
        return;

    m_vertical_tabs_expanded_width = clamped_width;
    update_tab_layout();
}

void TabWidget::persist_vertical_tabs_expanded_width()
{
    auto tab_settings = Application::settings().tab_settings();

    using ValueType = decltype(tab_settings.vertical_tabs_expanded_width)::ValueType;
    tab_settings.vertical_tabs_expanded_width = clamp(m_vertical_tabs_expanded_width, 0, NumericLimits<ValueType>::max());

    Application::settings().set_tab_settings(tab_settings);
}

void TabWidget::set_resize_handle_property(char const* property, bool enabled)
{
    set_dynamic_property_if_needed(*m_vertical_tab_bar_column, property, enabled);
}

void TabWidget::update_vertical_tabs_resize_handle()
{
    auto is_vertical = m_tab_bar->tab_layout() != TabLayout::Horizontal;
    // NB: The handle is a native view over the page, and shows what Qt drew behind it rather than the page. While the
    //     sidebar opens or closes, its edge moves away from the handle, so the handle stays hidden until it settles.
    auto show_resize_handle = m_tab_bar_visible && is_vertical && m_vertical_tabs_expanded && !m_vertical_tabs_toggling;
    m_vertical_tabs_resize_handle->setVisible(show_resize_handle);
    if (!show_resize_handle) {
        m_vertical_tabs_resize_handle->releaseMouse();
        if (m_is_resizing_vertical_tabs)
            QApplication::restoreOverrideCursor();
        m_is_resizing_vertical_tabs = false;
        set_resize_handle_property(VERTICAL_TABS_RESIZE_HANDLE_HOVERED_PROPERTY, false);
        set_resize_handle_property(VERTICAL_TABS_RESIZE_HANDLE_ACTIVE_PROPERTY, false);
        return;
    }

    auto handle_width = VERTICAL_TABS_RESIZE_HIT_AREA_WIDTH;
    auto divider_x = vertical_tabs_are_on_right() ? width() - vertical_tabs_layout_width() : vertical_tabs_layout_width();
    auto chrome_rect = vertical_tabs_chrome_rect();
    // NB: Keep the handle inside the sidebar. Native handles copy Qt's backing store, which does not contain the
    //     separately composited page on macOS.
    m_vertical_tabs_resize_handle->setGeometry(
        vertical_tabs_are_on_right() ? divider_x : divider_x - handle_width,
        chrome_rect.y(),
        handle_width,
        chrome_rect.height());
    m_vertical_tabs_resize_handle->raise();
}

void TabWidget::update_vertical_tabs_content_separator()
{
    auto* current = as_if<Tab>(m_stacked_widget->currentWidget());
    auto has_page_bookmarks_bar = current && current->has_bookmarks_bar_in_page();
    auto show_content_separator = m_tab_bar_visible && m_tab_bar->tab_layout() != TabLayout::Horizontal && !has_page_bookmarks_bar;
    m_vertical_tabs_content_separator->setVisible(show_content_separator);
    if (!show_content_separator)
        return;

    auto separator_x = vertical_tabs_are_on_right() ? 0 : max(0, current_vertical_tabs_width() - 1);
    auto separator_y = max(0, m_toolbar_container->height() - 1);
    auto separator_width = vertical_tabs_are_on_right() ? max(0, width() - current_vertical_tabs_width()) : max(0, width() - separator_x);
    m_vertical_tabs_content_separator->setGeometry(separator_x, separator_y, separator_width, 1);
    m_vertical_tabs_content_separator->raise();
}

void TabWidget::update_vertical_tabs_hover_layout()
{
    m_tab_bar->set_pinned_tabs_in_rows(m_vertical_tabs_hover_expanded && !m_vertical_tabs_expanded);
    m_tab_bar->set_tab_layout(current_tab_layout());

    m_tab_bar->set_vertical_tabs_position(m_vertical_tabs_position);
    set_dynamic_property_if_needed(*m_vertical_tab_bar_column, VERTICAL_TABS_POSITION_PROPERTY, vertical_tabs_position_property_value(m_vertical_tabs_position));
    update_vertical_tabs_column_margins();
    update_vertical_tabs_button_layout();

    update_tab_button_visibility();
    update_tab_layout();
}

QRect TabWidget::vertical_tabs_chrome_rect() const
{
    // NB: The open sidebar runs the full height of the window, and the collapsed one starts below the toolbar.
    auto chrome_height = m_vertical_tabs_expanded ? 0 : m_toolbar_container->height();
    auto chrome_width = current_vertical_tabs_width();
    return {
        vertical_tabs_are_on_right() ? max(0, width() - chrome_width) : 0,
        chrome_height,
        chrome_width,
        max(0, height() - chrome_height)
    };
}

int TabWidget::vertical_tabs_tab_width() const
{
    return max(0, current_vertical_tabs_width() - vertical_tabs_horizontal_margin_width(m_tab_bar->tab_layout()));
}

void TabWidget::update_vertical_tabs_button_layout()
{
    auto expanded = vertical_tabs_effectively_expanded();
    set_dynamic_property_if_needed(*m_new_tab_button, VERTICAL_TABS_BUTTON_PROPERTY, true);
    set_dynamic_property_if_needed(*m_new_tab_button, VERTICAL_TABS_EXPANDED_PROPERTY, expanded);
    auto tab_width = vertical_tabs_tab_width();
    auto button_size = QSize { tab_width, VERTICAL_TAB_HEIGHT };
    if (m_new_tab_button->minimumSize() != button_size || m_new_tab_button->maximumSize() != button_size)
        m_new_tab_button->setFixedSize(button_size);
    m_new_tab_button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

void TabWidget::update_tab_layout()
{
    if (m_tab_bar->tab_layout() != TabLayout::Horizontal) {
        m_all_tabs_button->hide();
        auto reserved_width = vertical_tabs_layout_width();
        auto side_bar_width = current_vertical_tabs_width();
        m_vertical_tabs_reserved_space->setFixedWidth(reserved_width);
        m_vertical_tab_bar_column->setFixedWidth(side_bar_width);
        set_dynamic_property_if_needed(*m_vertical_tab_bar_column, VERTICAL_TABS_POSITION_PROPERTY, vertical_tabs_position_property_value(m_vertical_tabs_position));
        update_vertical_tabs_button_layout();
        update_vertical_tabs_overlay_geometry();
        m_vertical_tabs_content_layout->activate();
        m_tab_bar->set_available_width(vertical_tabs_tab_width());
        update_vertical_tabs_resize_handle();
        update_vertical_tabs_content_separator();
        return;
    }

    update_vertical_tabs_resize_handle();
    update_vertical_tabs_content_separator();

    // Everything on the strip but the tab bar: its margins, the window controls, and the new tab button with its gap.
    auto margins = m_tab_bar_row_layout->contentsMargins();
    auto controls_width = margins.left() + margins.right() + HORIZONTAL_NEW_TAB_BUTTON_GAP - HORIZONTAL_TAB_END_ROOM + HORIZONTAL_NEW_TAB_BUTTON_SIZE + horizontal_tab_strip_drag_space();
    controls_width += m_ladybird_button->width() + LADYBIRD_BUTTON_TRAILING_GAP - HORIZONTAL_TAB_END_ROOM;
    if (!m_window_controls->isHidden())
        controls_width += m_window_controls->width() + (use_native_macos_window_controls() ? LADYBIRD_BUTTON_TRAFFIC_LIGHTS_GAP : 0);

    auto available_for_tabs = width() - controls_width;

    // Tabs that overflow the strip even at their narrowest make room for the button that lists them all.
    auto pinned_count = m_tab_bar->pinned_tab_count();
    auto narrowest_tabs_width = (pinned_count * HORIZONTAL_PINNED_TAB_WIDTH) + ((m_tab_bar->count() - pinned_count) * HORIZONTAL_TAB_MIN_WIDTH) + (2 * HORIZONTAL_TAB_END_ROOM);
    auto tabs_overflow = narrowest_tabs_width > available_for_tabs;
    if (tabs_overflow)
        available_for_tabs -= HORIZONTAL_NEW_TAB_BUTTON_SIZE;
    if (m_all_tabs_button->parentWidget() != m_tab_bar_row)
        m_all_tabs_button->setParent(m_tab_bar_row);
    m_all_tabs_button->setVisible(tabs_overflow && m_tab_bar_visible);

    // NB: The tab strip has no minimum width from its contents (see the constructor), so it keeps the window wide enough
    //     for its controls and one narrow tab with a minimum of its own.
    m_tab_bar_row->setMinimumWidth(controls_width + HORIZONTAL_TAB_MIN_WIDTH + (2 * HORIZONTAL_TAB_END_ROOM));

    m_tab_bar->set_available_width(available_for_tabs);

    auto tabs_width = (pinned_count * HORIZONTAL_PINNED_TAB_WIDTH) + ((m_tab_bar->count() - pinned_count) * HORIZONTAL_TAB_MAX_WIDTH) + (2 * HORIZONTAL_TAB_END_ROOM);
    auto tab_bar_width = min(available_for_tabs, tabs_width + max(0, m_tab_bar->horizontal_tabs_end_offset()));
    m_tab_bar->setFixedWidth(max(0, tab_bar_width));
    place_horizontal_new_tab_button();
}

void TabWidget::update_tab_button_visibility()
{
    if (m_tab_bar->tab_layout() == TabLayout::VerticalCollapsed) {
        for (int index = 0; index < m_tab_bar->count(); ++index) {
            if (auto* button = m_tab_bar->tabButton(index, QTabBar::RightSide))
                button->hide();
        }

        m_tab_bar->refresh_tab_layout();
        return;
    }

    for (int index = 0; index < m_tab_bar->count(); ++index) {
        if (auto* button = m_tab_bar->tabButton(index, QTabBar::LeftSide))
            button->setVisible(true);
        if (auto* button = m_tab_bar->tabButton(index, QTabBar::RightSide))
            button->setVisible(true);
    }

    m_tab_bar->refresh_tab_layout();
}

void TabWidget::update_tab_chrome_visibility()
{
    auto tab_layout = m_tab_bar->tab_layout();
    auto is_horizontal = tab_layout == TabLayout::Horizontal;
    auto show_top_row = m_tab_bar_visible && is_horizontal;
    m_tab_bar_row->setVisible(show_top_row);
    m_toolbar_container->setVisible(m_tab_bar_visible);
    m_toolbar_placeholder->setVisible(m_tab_bar_visible && m_toolbar_follows_page_column);
    m_vertical_tab_bar_column->setVisible(m_tab_bar_visible && !is_horizontal);
    update_vertical_tabs_content_separator();

    auto show_window_controls = m_window_controls_visible
        && m_tab_bar_visible
        && is_horizontal;
    m_window_controls->setVisible(show_window_controls);
}

void TabWidget::recreate_icons()
{
    m_tab_bar->recreate_icons();
    update_vertical_tabs_toggle_icon();
    auto activity = ChromeStyle::window_activity(*this);
    m_new_tab_button->setIcon(create_chrome_icon(ChromeIcon::NewTab, Theme::chrome_palette(*this), activity));
    m_all_tabs_button->setIcon(create_chrome_icon(ChromeIcon::ChevronDown, Theme::chrome_palette(*this), activity));
    update_window_button_icons();
}

void TabWidget::update_vertical_tabs_toggle_icon()
{
    // NB: The icons are first made before the sidebar and its toggle are.
    if (!m_vertical_tabs_toggle_button)
        return;
    auto icon = vertical_tabs_are_on_right() ? ChromeIcon::SidebarRight : ChromeIcon::Sidebar;
    m_vertical_tabs_toggle_button->defaultAction()->setIcon(create_chrome_icon(icon, Theme::chrome_palette(*this), ChromeStyle::window_activity(*this)));
}

void TabWidget::update_chrome_style()
{
    if (m_is_updating_chrome_style)
        return;

    m_is_updating_chrome_style = true;
    auto style_sheet = ChromeStyle::tab_widget_style_sheet(Theme::chrome_palette(*this));
    m_tab_bar_row->setStyleSheet(style_sheet);
    m_vertical_tab_bar_column->setStyleSheet(style_sheet);
    m_vertical_tabs_content_separator->setStyleSheet(style_sheet);
    m_vertical_tabs_resize_handle->setStyleSheet(style_sheet);
    auto reserved_space_palette = m_vertical_tabs_reserved_space->palette();
    reserved_space_palette.setColor(QPalette::Window, ChromeStyle::chrome_sidebar_background(Theme::chrome_palette(*this)));
    m_vertical_tabs_reserved_space->setPalette(reserved_space_palette);
    if (m_all_tabs_popover)
        m_all_tabs_popover->update_chrome_style(palette());
    m_is_updating_chrome_style = false;
}

void TabWidget::update_vertical_tabs_overlay_geometry()
{
    if (!m_tab_bar_visible || m_tab_bar->tab_layout() == TabLayout::Horizontal) {
        m_vertical_tab_bar_column->hide();
        return;
    }

    m_vertical_tab_bar_column->setGeometry(vertical_tabs_chrome_rect());
    m_vertical_tab_bar_column->show();
    m_vertical_tab_bar_column->raise();
}

void TabWidget::set_vertical_tabs_hover_expanded(bool expanded)
{
    m_vertical_tabs_hover_expand_timer->stop();
    expanded &= can_expand_vertical_tabs_on_hover();
    if (m_vertical_tabs_hover_expanded == expanded)
        return;

    m_vertical_tabs_hover_expanded = expanded;
    m_vertical_tabs_hover_expand_animation->stop();

    auto duration = m_vertical_tabs_hover_expanded && !SystemAppearance::prefers_reduced_motion()
        ? Application::settings().config_variable_as_integer<int>(WebView::ConfigVariableID::VerticalTabsHoverExpandAnimationDuration)
        : 0;
    m_vertical_tabs_hover_expand_progress = m_vertical_tabs_hover_expanded && duration == 0 ? 1.0 : 0.0;

    if (m_vertical_tabs_hover_expanded)
        m_vertical_tabs_hover_collapse_timer->start();
    else
        m_vertical_tabs_hover_collapse_timer->stop();

    update_vertical_tabs_hover_layout();
    update_vertical_tabs_content_overlay();

    if (m_vertical_tabs_hover_expanded && duration > 0) {
        m_vertical_tabs_hover_expand_animation->setDuration(duration);
        m_vertical_tabs_hover_expand_animation->start();
    }
}

void TabWidget::update_vertical_tabs_content_overlay()
{
    auto index = current_index();
    if (index < 0)
        return;

    int overlap = 0;
    if (m_tab_bar_visible && m_vertical_tabs_hover_expanded && m_tab_bar->tab_layout() != TabLayout::Horizontal)
        overlap = max(0, current_vertical_tabs_width() - vertical_tabs_layout_width());

    auto left = vertical_tabs_are_on_right() ? 0 : overlap;
    auto right = vertical_tabs_are_on_right() ? overlap : 0;

    if (auto* current = tab(index))
        current->view().set_vertical_tab_overlay_insets(left, right);
}

void TabWidget::defer_update_vertical_tabs_hover_expanded()
{
    QTimer::singleShot(0, this, [this]() {
        update_vertical_tabs_hover_expanded();
    });
}

void TabWidget::update_vertical_tabs_hover_expanded()
{
    if (!m_vertical_tabs_hover_expanded && !m_vertical_tabs_hover_expand_timer->isActive())
        return;

    if (!cursor_is_over_vertical_tabs())
        set_vertical_tabs_hover_expanded(false);
}

void TabWidget::update_window_button_icons()
{
    if (m_minimize_window_button && m_maximize_window_button && m_close_window_button) {
        auto is_maximized = window()->isMaximized();
        m_minimize_window_button->setIcon(create_chrome_icon(ChromeIcon::WindowMinimize, Theme::chrome_palette(*this)));
        m_maximize_window_button->setIcon(create_chrome_icon(is_maximized ? ChromeIcon::WindowRestore : ChromeIcon::WindowMaximize, Theme::chrome_palette(*this)));
        m_maximize_window_button->setToolTip(is_maximized ? "Restore" : "Maximize");
        m_close_window_button->setIcon(create_chrome_icon(ChromeIcon::WindowClose, Theme::chrome_palette(*this)));
    }

    for (int index = 0; index < m_stacked_widget->count(); ++index)
        tab(index)->update_window_control_icons();
}

void TabWidget::toggle_window_maximized()
{
    auto* top_level_window = window();
    if (top_level_window->isMaximized())
        top_level_window->showNormal();
    else
        top_level_window->showMaximized();
    update_window_button_icons();
}

bool TabWidget::start_window_move()
{
    auto* handle = window()->windowHandle();
    if (!handle)
        return false;
#if defined(AK_OS_MACOS)
    if (start_appkit_window_drag(*this))
        return true;
#endif
    return handle->startSystemMove();
}

void TabBarButton::set_placement(Placement placement)
{
    if (m_placement == placement)
        return;

    m_placement = placement;
    update_size();
    update();
}

TabBarButton::TabBarButton(QIcon const& icon, Role role, QWidget* parent)
    : QPushButton(icon, {}, parent)
    , m_role(role)
{
    setObjectName("LadybirdTabButton");
    update_size();
    setFocusPolicy(Qt::NoFocus);
    setFlat(true);
    hide();
}

void TabBarButton::update_size()
{
    auto icon_size = TAB_BUTTON_ICON_SIZE;
    switch (m_placement) {
    case Placement::Tab:
    case Placement::SidebarRow:
        if (m_role == Role::Close) {
            setFixedSize(TAB_CLOSE_BUTTON_HIT_SIZE);
        } else if (m_placement == Placement::SidebarRow) {
            setFixedSize(SIDEBAR_ROW_AUDIO_BUTTON_SIZE, SIDEBAR_ROW_AUDIO_BUTTON_SIZE);
            icon_size = SIDEBAR_ROW_AUDIO_ICON_SIZE;
        } else {
            setFixedSize(TAB_AUDIO_BUTTON_SIZE, TAB_AUDIO_BUTTON_SIZE);
        }
        break;
    case Placement::RailBadge:
        setFixedSize(RAIL_BADGE_SIZE, RAIL_BADGE_SIZE);
        icon_size = m_role == Role::Close ? RAIL_BADGE_CLOSE_ICON_SIZE : RAIL_BADGE_AUDIO_ICON_SIZE;
        break;
    }
    setIconSize({ icon_size, icon_size });
}

// The close button fades into the slot its tab keeps for it, and out of it, so that nothing else on the tab moves.
// NB: Other layouts show and hide the button outright, so it is left opaque whenever it is not fading.
void TabBarButton::set_revealed(bool revealed)
{
    auto is_fading = m_reveal_animation && m_reveal_animation->state() == QAbstractAnimation::Running;
    if (revealed == m_revealed && (is_fading || isVisible() == revealed))
        return;
    auto was_revealed = m_revealed;
    m_revealed = revealed;

    // NB: A button that was never revealed can still show, as when its tab bar shows all of its buttons to lay them
    //     out again. It hides at once rather than fading out, so that it never appears.
    if (!revealed && (!isVisible() || (!was_revealed && !is_fading))) {
        if (isVisible())
            hide();
        if (m_reveal_animation)
            m_reveal_animation->stop();
        m_opacity = 1;
        return;
    }

    if (SystemAppearance::prefers_reduced_motion()) {
        m_opacity = 1;
        setVisible(revealed);
        return;
    }

    if (!m_reveal_animation) {
        m_reveal_animation = new QVariantAnimation(this);
        m_reveal_animation->setDuration(Motion::FAST);
        m_reveal_animation->setEasingCurve(Motion::move_easing());
        QObject::connect(m_reveal_animation, &QVariantAnimation::valueChanged, this, [this](QVariant const& value) {
            m_opacity = value.toReal();
            update();
        });
        QObject::connect(m_reveal_animation, &QVariantAnimation::finished, this, [this] {
            if (!m_revealed)
                hide();
            m_opacity = 1;
        });
    }

    auto from = isVisible() ? m_opacity : 0.0;
    m_reveal_animation->stop();
    m_reveal_animation->setStartValue(from);
    m_reveal_animation->setEndValue(revealed ? 1.0 : 0.0);
    m_opacity = from;
    if (revealed)
        show();
    m_reveal_animation->start();
}

bool TabBarButton::event(QEvent* event)
{
    if (event->type() == QEvent::Enter)
        setFlat(false);
    if (event->type() == QEvent::Leave)
        setFlat(true);

    return QPushButton::event(event);
}

void TabBarButton::paintEvent(QPaintEvent*)
{
    QStyleOptionButton option;
    initStyleOption(&option);

    // The speaker in the sidebar and both badges in the collapsed sidebar take the icon color, rather than the tabs'
    // quieter one.
    auto takes_icon_color = m_placement == Placement::RailBadge || (m_placement == Placement::SidebarRow && m_role == Role::Audio);
    if (takes_icon_color && isEnabled() && ChromeStyle::window_activity(*this) == ChromeStyle::WindowActivity::Active)
        option.icon = QIcon(icon().pixmap(iconSize(), devicePixelRatioF(), QIcon::Active));

    QStylePainter painter(this);
    painter.setOpacity(m_opacity);

    if (m_placement == Placement::RailBadge) {
        ChromeStyle::paint_control_frame(painter, *this, ChromeStyle::ControlFrameStyle::ActiveTabOverlay);
    } else if (m_role == Role::Close) {
        if (isDown() || (underMouse() && rect().contains(mapFromGlobal(QCursor::pos())))) {
            // A tab's close button is a small rounded square that lights up over the tab's own fill.
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.setPen(Qt::NoPen);
            painter.setBrush(isDown() ? Theme::panel().pressed : Theme::chrome(*this).hover);
            QRectF square { (width() - TAB_CLOSE_BUTTON_SIZE) / 2.0, (height() - TAB_CLOSE_BUTTON_SIZE) / 2.0, TAB_CLOSE_BUTTON_SIZE, TAB_CLOSE_BUTTON_SIZE };
            painter.drawRoundedRect(square, TAB_BUTTON_RADIUS, TAB_BUTTON_RADIUS);
        }
    } else {
        ChromeStyle::paint_control_frame(painter, *this);
    }

    painter.drawControl(QStyle::CE_PushButton, option);
}

}
