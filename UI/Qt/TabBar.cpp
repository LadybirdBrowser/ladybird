/*
 * Copyright (c) 2024-2026, Tim Flynn <trflynn89@ladybird.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/EnumBits.h>
#include <AK/ScopeGuard.h>
#include <AK/StdLibExtras.h>
#include <AK/kmalloc.h>
#include <LibCore/EventLoop.h>
#include <UI/Qt/Application.h>
#include <UI/Qt/BrowserWindow.h>
#include <UI/Qt/ChromeLayout.h>
#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/Icon.h>
#if defined(AK_OS_MACOS)
#    include <UI/Qt/MacWindow.h>
#endif
#include <UI/Qt/HiddenPageSizing.h>
#include <UI/Qt/Menu.h>
#include <UI/Qt/StringUtils.h>
#include <UI/Qt/TabBar.h>
#include <UI/Qt/WindowControlButton.h>

#include <QApplication>
#include <QContextMenuEvent>
#include <QCursor>
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
static constexpr int HORIZONTAL_TAB_STRIP_HEIGHT = 39;
// Horizontal tabs reach the bottom of the strip, where the selected tab flows into the toolbar below it.
static constexpr int HORIZONTAL_TAB_HEIGHT = 37;
// Horizontal tabs shrink until only their favicon is left, then overflow the strip and get clipped.
static constexpr int HORIZONTAL_TAB_MIN_WIDTH = 40;
// Narrower background tabs drop their close button to keep room for the title.
static constexpr int HORIZONTAL_TAB_CLOSE_BUTTON_MIN_WIDTH = 96;
// The active tab keeps its close button for as long as it fits next to the favicon.
static constexpr int HORIZONTAL_ACTIVE_TAB_CLOSE_BUTTON_MIN_WIDTH = 68;
// Below this content width, a horizontal tab shows its favicon alone.
static constexpr int HORIZONTAL_TAB_MIN_CONTENT_WIDTH_FOR_TITLE = 48;
static constexpr int HORIZONTAL_TAB_MAX_WIDTH = 232;
// A pinned tab's tile, which shows only its favicon.
static constexpr int PINNED_TAB_TILE_WIDTH = 40;
static constexpr int PINNED_TABS_SEPARATOR_HEIGHT = 9;
static constexpr int VERTICAL_TAB_HEIGHT = 37;
static constexpr int VERTICAL_TABS_COLLAPSED_WIDTH = browser_chrome_layout_policy().collapsed_sidebar_width;
static constexpr int VERTICAL_TABS_DEFAULT_EXPANDED_WIDTH = browser_chrome_layout_policy().expanded_sidebar_width;
static constexpr int VERTICAL_TABS_MIN_EXPANDED_WIDTH = 190;
static constexpr int VERTICAL_TABS_MAX_EXPANDED_WIDTH = 400;
static constexpr int VERTICAL_TABS_RESIZE_HIT_AREA_WIDTH = 5;
static constexpr int VERTICAL_TABS_COLLAPSED_SIDE_MARGIN = 6;
static constexpr int VERTICAL_TABS_EXPANDED_SIDE_MARGIN = 5;
static constexpr int VERTICAL_TABS_TOP_MARGIN = 8;
static constexpr int VERTICAL_TABS_TOGGLE_BUTTON_SIZE = 32;
// The collapsed sidebar's corner below the toolbar, beside the page, is as round as the page's own corners.
static constexpr qreal VERTICAL_TABS_COLLAPSED_CORNER_RADIUS = 10.0;
static constexpr int TAB_CARD_SHAPE_HORIZONTAL_INSET = 5;
// A vertical tab's card sits a point higher than the middle of its row, so the cards in the sidebar sit close together
// while they keep an even height.
static constexpr int TAB_CARD_SHAPE_TOP_INSET = 3;
static constexpr int TAB_CARD_SHAPE_BOTTOM_INSET = 2;
// The selected vertical tab reaches across to the page's frame, with its sides flaring out into it above and below. The
// bar leaves this much room above its first tab and below its last, so the flares there are not cut off.
static constexpr int VERTICAL_TAB_FLARE_RADIUS = 6;
// A horizontal tab's card holds its contents, and is what lights up when the tab is hovered. The selected tab is its card
// extended down to the toolbar. The card sits above the middle of the tab, so its contents line up with the window
// controls.
static constexpr qreal HORIZONTAL_TAB_CARD_SHAPE_HORIZONTAL_INSET = 2.5;
static constexpr int HORIZONTAL_TAB_CARD_SHAPE_TOP_INSET = 4;
static constexpr int HORIZONTAL_TAB_CARD_SHAPE_BOTTOM_INSET = 5;
static constexpr qreal HORIZONTAL_TAB_CARD_RADIUS = (HORIZONTAL_TAB_HEIGHT - HORIZONTAL_TAB_CARD_SHAPE_TOP_INSET - HORIZONTAL_TAB_CARD_SHAPE_BOTTOM_INSET) / 2.0;
// The new tab button's circle is as tall as a tab's card, so its top is level with the top of the selected tab.
static constexpr qreal HORIZONTAL_NEW_TAB_BUTTON_SHAPE_SIZE = 2 * HORIZONTAL_TAB_CARD_RADIUS;
// The selected horizontal tab's sides flare out at the bottom into the toolbar. Each flare reaches into the tab beside
// it, which has no fill of its own, and curves around the end of that tab's card at a constant distance.
// NB: For that curve to also meet the toolbar in a tangent, the distance from the card's middle to the bottom of the tab
//     must equal the curve's radius, which the card's insets are chosen for.
static constexpr qreal HORIZONTAL_TAB_FLARE_RADIUS = HORIZONTAL_TAB_CARD_RADIUS + (2 * HORIZONTAL_TAB_CARD_SHAPE_HORIZONTAL_INSET);
static_assert(HORIZONTAL_TAB_FLARE_RADIUS == HORIZONTAL_TAB_HEIGHT - HORIZONTAL_TAB_CARD_SHAPE_TOP_INSET - HORIZONTAL_TAB_CARD_RADIUS);
// The first and last tabs are this much wider, on the outer side, to make room for their flares at the ends of the tab
// bar.
static constexpr int HORIZONTAL_TAB_END_ROOM = 17;
static_assert(HORIZONTAL_TAB_END_ROOM + HORIZONTAL_TAB_CARD_SHAPE_HORIZONTAL_INSET >= HORIZONTAL_TAB_FLARE_RADIUS);
// The new tab button sits over the last tab's room for its flare, with its circle centered where the flare's curve is, as
// a tab's card beside the last tab would be. The curve then keeps clear of the circle.
static constexpr int HORIZONTAL_NEW_TAB_BUTTON_OVERLAP = static_cast<int>(HORIZONTAL_TAB_END_ROOM + HORIZONTAL_TAB_CARD_SHAPE_HORIZONTAL_INSET - HORIZONTAL_TAB_FLARE_RADIUS + (HORIZONTAL_TAB_HEIGHT / 2.0));
static constexpr qreal HORIZONTAL_TAB_SEPARATOR_WIDTH = 1.5;
static constexpr qreal HORIZONTAL_TAB_SEPARATOR_HEIGHT = 20.0;
static constexpr int TAB_CONTENT_HORIZONTAL_INSET = 8;
static constexpr int VERTICAL_TABS_HOVER_COLLAPSE_POLL_INTERVAL_MS = 250;
static constexpr int TAB_PREVIEW_HOVER_DELAY_MS = 350;
static constexpr int TAB_PREVIEW_THUMBNAIL_WIDTH = 320;
static constexpr int TAB_PREVIEW_THUMBNAIL_HEIGHT = 180;
static constexpr int TAB_PREVIEW_SHADOW_MARGIN = 12;
static constexpr int TAB_PREVIEW_CARD_GAP = 6;
static constexpr int TAB_BUTTON_SIZE = 22;
static constexpr int TAB_BUTTON_ICON_SIZE = 14;
static constexpr qreal LOAD_SHEEN_FADE_PROGRESS = 0.15;
static constexpr int LOAD_SHEEN_DURATION_MS = 400;
static constexpr int DROP_PREVIEW_DURATION_MS = 150;
static constexpr int TAB_TITLE_FADE_WIDTH = 24;
static constexpr int COLLAPSED_VERTICAL_TAB_BUTTON_SIZE = 16;
static constexpr int COLLAPSED_VERTICAL_TAB_ICON_SIZE = 12;
static constexpr auto COLLAPSED_VERTICAL_TAB_BUTTON_PROPERTY = "collapsedVerticalTabButton";
static constexpr auto FULL_WIDTH_TOOLBAR_PROPERTY = "fullWidthToolbar";
static constexpr auto VERTICAL_TABS_EXPANDED_PROPERTY = "verticalTabsExpanded";
static constexpr auto VERTICAL_TABS_BUTTON_PROPERTY = "verticalTabsButton";
static constexpr auto VERTICAL_TABS_POSITION_PROPERTY = "verticalTabsPosition";
static constexpr auto WINDOW_INACTIVE_PROPERTY = "windowInactive";
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

static constexpr int clamp_vertical_tabs_expanded_width(int width)
{
    return clamp(width, VERTICAL_TABS_MIN_EXPANDED_WIDTH, VERTICAL_TABS_MAX_EXPANDED_WIDTH);
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

enum class TabBarEdge : u8 {
    None = 0,
    Left = 1 << 0,
    Right = 1 << 1,
};
AK_ENUM_BITWISE_OPERATORS(TabBarEdge);

static TabBarEdge tab_bar_edges(int index, int count)
{
    auto edges = TabBarEdge::None;
    if (index == 0)
        edges |= TabBarEdge::Left;
    if (index == count - 1)
        edges |= TabBarEdge::Right;
    return edges;
}

static QRectF horizontal_tab_card_shape_rect(QRectF const& rect, TabBarEdge edges = TabBarEdge::None)
{
    auto left_inset = HORIZONTAL_TAB_CARD_SHAPE_HORIZONTAL_INSET + (has_flag(edges, TabBarEdge::Left) ? HORIZONTAL_TAB_END_ROOM : 0);
    auto right_inset = HORIZONTAL_TAB_CARD_SHAPE_HORIZONTAL_INSET + (has_flag(edges, TabBarEdge::Right) ? HORIZONTAL_TAB_END_ROOM : 0);
    return rect.adjusted(left_inset, HORIZONTAL_TAB_CARD_SHAPE_TOP_INSET, -right_inset, -HORIZONTAL_TAB_CARD_SHAPE_BOTTOM_INSET);
}

// The selected horizontal tab's outline, from the bottom of its left flare, over the top of its body, to the bottom of
// its right flare. The path is left open, so the loading swirl does not run along the bottom edge, where the tab meets
// the toolbar. Filling it closes it.
static QPainterPath horizontal_selected_tab_path(QRectF const& body)
{
    // NB: The top corners are as round as a card's ends.
    auto top_radius = HORIZONTAL_TAB_CARD_RADIUS;
    auto flare = HORIZONTAL_TAB_FLARE_RADIUS;
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

// The selected vertical tab's outline: Its card reaches across to the page's edge of the sidebar at x = page_edge, and
// flares out above and below into the page's frame there. As with the horizontal tab, the path is left open along the
// edge it shares with the frame, and runs clockwise, which is the way the loading swirl goes around it. Filling it closes
// it.
static QPainterPath vertical_selected_tab_path(QRectF const& card, qreal page_edge)
{
    auto top = card.top();
    auto bottom = card.bottom();
    auto radius = card.height() / 2.0;
    qreal flare = VERTICAL_TAB_FLARE_RADIUS;

    QPainterPath path;
    if (page_edge > card.center().x()) {
        // From the bottom of the page's edge, along the bottom, around the round end, and along the top.
        path.moveTo(page_edge, bottom + flare);
        path.arcTo(QRectF(page_edge - (2 * flare), bottom, 2 * flare, 2 * flare), 0, 90);
        path.lineTo(card.left() + radius, bottom);
        path.arcTo(QRectF(card.left(), top, 2 * radius, 2 * radius), 270, -180);
        path.lineTo(page_edge - flare, top);
        path.arcTo(QRectF(page_edge - (2 * flare), top - (2 * flare), 2 * flare, 2 * flare), 270, 90);
        return path;
    }

    // From the top of the page's edge, along the top, around the round end, and along the bottom.
    path.moveTo(page_edge, top - flare);
    path.arcTo(QRectF(page_edge, top - (2 * flare), 2 * flare, 2 * flare), 180, 90);
    path.lineTo(card.right() - radius, top);
    path.arcTo(QRectF(card.right() - (2 * radius), top, 2 * radius, 2 * radius), 90, -180);
    path.lineTo(page_edge + flare, bottom);
    path.arcTo(QRectF(page_edge, bottom, 2 * flare, 2 * flare), 90, 90);
    return path;
}

// The new tab button lines up with the cards of the tabs beside it.
static QRectF horizontal_new_tab_button_shape_rect(QRectF const& rect)
{
    auto center = horizontal_tab_card_shape_rect(rect).center();
    auto half_size = HORIZONTAL_NEW_TAB_BUTTON_SHAPE_SIZE / 2.0;
    return { center.x() - half_size, center.y() - half_size, HORIZONTAL_NEW_TAB_BUTTON_SHAPE_SIZE, HORIZONTAL_NEW_TAB_BUTTON_SHAPE_SIZE };
}

static QRect tab_card_shape_rect(QRect const& rect)
{
    return rect.adjusted(TAB_CARD_SHAPE_HORIZONTAL_INSET, TAB_CARD_SHAPE_TOP_INSET, -TAB_CARD_SHAPE_HORIZONTAL_INSET, -TAB_CARD_SHAPE_BOTTOM_INSET);
}

// When a tab finishes loading, a soft band of light sweeps across its pill from left to right.
static void paint_load_sheen(QPainter& painter, QPalette const& palette, QPainterPath const& tab_path, QRectF const& shape_rect, qreal progress)
{
    // The bright middle of the band travels from the left edge of the tab to its right edge, so the sweep is seen to
    // start at the very left. It fades out at the right edge rather than leaving the tab.
    auto band_width = shape_rect.height() * 2.5;
    auto center_x = shape_rect.left() + (shape_rect.width() * progress);
    auto strength = min(1.0, (1.0 - progress) / LOAD_SHEEN_FADE_PROGRESS);

    // NB: The gradient runs from the top left to the bottom right of the band, which slants the band like "/".
    QLinearGradient sheen(QPointF(center_x - (band_width / 2.0), shape_rect.top()), QPointF(center_x + (band_width / 2.0), shape_rect.bottom()));
    auto peak = ChromeStyle::is_dark(palette) ? QColor(255, 255, 255, 46) : QColor(0, 0, 0, 14);
    peak.setAlphaF(peak.alphaF() * strength);
    auto clear = peak;
    clear.setAlpha(0);
    sheen.setColorAt(0.0, clear);
    sheen.setColorAt(0.5, peak);
    sheen.setColorAt(1.0, clear);

    painter.save();
    painter.setClipPath(tab_path, Qt::IntersectClip);
    painter.fillRect(shape_rect, sheen);
    painter.restore();
}

static QColor tab_hover_surface(QPalette const& palette, qreal hover_progress)
{
    auto dark = ChromeStyle::is_dark(palette);
    auto color = dark ? QColor(255, 255, 255) : QColor(0, 0, 0);
    color.setAlpha(static_cast<int>((dark ? 24 : 22) * hover_progress));
    return color;
}

static QColor horizontal_tab_separator(QWidget const& widget)
{
    auto const& palette = widget.palette();
    auto dark = ChromeStyle::is_dark(palette);
    auto background = ChromeStyle::chrome_tab_strip_background(palette, ChromeStyle::window_activity(widget));
    return ChromeStyle::mix(background, ChromeStyle::chrome_text(palette), dark ? 0.18 : 0.25);
}

static QRectF collapsed_vertical_tab_shape_rect(QRectF const& rect)
{
    return rect.adjusted(4.0, TAB_CARD_SHAPE_TOP_INSET, -4.0, -TAB_CARD_SHAPE_BOTTOM_INSET);
}

static QRect collapsed_vertical_tab_shape_rect(QRect const& rect)
{
    return rect.adjusted(4, TAB_CARD_SHAPE_TOP_INSET, -4, -TAB_CARD_SHAPE_BOTTOM_INSET);
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

    virtual void paintEvent(QPaintEvent* event) override
    {
        if (!property(VERTICAL_TABS_BUTTON_PROPERTY).toBool()) {
            QToolButton::paintEvent(event);
            return;
        }

        auto expanded = property(VERTICAL_TABS_EXPANDED_PROPERTY).toBool();

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        auto dark = ChromeStyle::is_dark(palette());
        auto is_horizontal_new_tab_button = m_tab_bar.tab_layout() == TabLayout::Horizontal;
        auto shape_rect = expanded
            ? tab_card_shape_rect(QRectF(rect()))
            : (is_horizontal_new_tab_button
                      ? horizontal_new_tab_button_shape_rect(QRectF(rect()))
                      : QRectF(rect()).adjusted(4.0, TAB_CARD_SHAPE_TOP_INSET, -4.0, -TAB_CARD_SHAPE_BOTTOM_INSET));
        // NB: The horizontal new tab button's shape is square, so this pill is a circle.
        QPainterPath tab_path;
        tab_path.addRoundedRect(shape_rect, shape_rect.height() / 2.0, shape_rect.height() / 2.0);
        // NB: Hovered and pressed, the button looks like the toolbar's buttons, though in the shape of a tab.
        auto hovered = underMouse() && rect().contains(mapFromGlobal(QCursor::pos()));
        if (auto colors = ChromeStyle::circular_control_frame_colors(*this); colors.has_value()) {
            // NB: Hovered but not pressed, it lights up like the tabs beside it.
            if (hovered && !isDown())
                colors->background = tab_hover_surface(palette(), 1.0);
            painter.setBrush(colors->background);
            painter.setPen(colors->border.alpha() > 0 ? QPen(colors->border, 1) : QPen(Qt::NoPen));
            painter.drawPath(tab_path);
        }

        auto contents_rect = shape_rect.toAlignedRect().adjusted(TAB_CONTENT_HORIZONTAL_INSET, 0, -TAB_CONTENT_HORIZONTAL_INSET, 0);
        auto icon_size = QSize(TAB_ICON_SIZE, TAB_ICON_SIZE);
        QRect icon_rect {
            expanded ? contents_rect.left() - ((icon_size.width() - TAB_ICON_SIZE) / 2) : rect().center().x() - (icon_size.width() / 2),
            contents_rect.top() + ((contents_rect.height() - icon_size.height()) / 2),
            icon_size.width(),
            icon_size.height(),
        };
        if (is_horizontal_new_tab_button) {
            painter.translate(0.5, 0);
        } else if (!expanded) {
            painter.translate(1.0, 0);
        }
        icon().paint(&painter, icon_rect);
        if (!expanded)
            return;

        contents_rect.setLeft(icon_rect.right() + 8);

        auto text_color = ChromeStyle::mix(ChromeStyle::chrome_button_text(palette()), ChromeStyle::chrome_muted_text(palette()), dark ? 0.26 : 0.40);
        if (hovered || isDown())
            text_color = ChromeStyle::chrome_button_text(palette());
        painter.setPen(text_color);
        auto text_font = ChromeStyle::chrome_title_font(m_tab_bar.font());
        painter.setFont(text_font);

        QFontMetrics font_metrics(text_font);
        auto title = font_metrics.elidedText(text(), Qt::ElideRight, max(0, contents_rect.width()));
        contents_rect.translate(0, -1);
        painter.drawText(contents_rect, Qt::AlignLeft | Qt::AlignVCenter, title);
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
        ChromeStyle::paint_circular_control_frame(painter, *this);
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
        : QWidget(parent, Qt::ToolTip | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint)
    {
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);

        auto* outer_layout = new QVBoxLayout(this);
        outer_layout->setContentsMargins(TAB_PREVIEW_SHADOW_MARGIN, TAB_PREVIEW_SHADOW_MARGIN, TAB_PREVIEW_SHADOW_MARGIN, TAB_PREVIEW_SHADOW_MARGIN);
        outer_layout->setSpacing(0);

        m_card = new QFrame(this);
        m_card->setObjectName("LadybirdTabPreviewCard");

        auto* shadow = new QGraphicsDropShadowEffect(m_card);
        shadow->setBlurRadius(28);
        shadow->setOffset(0, 8);
        shadow->setColor(QColor(0, 0, 0, 92));
        m_card->setGraphicsEffect(shadow);

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
    bool was_canceled() const { return m_was_canceled; }

private:
    virtual bool eventFilter(QObject*, QEvent* event) override
    {
        if (event->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape)
            m_was_canceled = true;
        return false;
    }

    bool m_was_canceled { false };
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

    m_tab_preview_timer = new QTimer(this);
    m_tab_preview_timer->setInterval(TAB_PREVIEW_HOVER_DELAY_MS);
    m_tab_preview_timer->setSingleShot(true);
    connect(m_tab_preview_timer, &QTimer::timeout, this, &TabBar::show_tab_preview);

    m_tab_preview_popup = new TabPreviewPopup(this);

    m_loading_swirl_animation = new QVariantAnimation(this);
    m_loading_swirl_animation->setDuration(ChromeStyle::LOADING_SWIRL_PERIOD_MS);
    m_loading_swirl_animation->setStartValue(0.0);
    m_loading_swirl_animation->setEndValue(1.0);
    m_loading_swirl_animation->setLoopCount(-1);
    // NB: Each frame repaints only the loading tabs, and checks again, so the swirl also stops when the last loading tab
    //     is closed.
    connect(m_loading_swirl_animation, &QVariantAnimation::valueChanged, this, [this] {
        auto any_tab_loading = false;
        for (int index = 0; index < count(); ++index) {
            if (!is_tab_loading(index))
                continue;
            any_tab_loading = true;
            // NB: The selected horizontal tab's flares, which the swirl runs along, reach past its rect.
            auto flare = qCeil(HORIZONTAL_TAB_FLARE_RADIUS);
            update(visual_tab_rect(index).adjusted(-flare, 0, flare, 0));
        }
        if (!any_tab_loading || !isVisible())
            update_loading_swirl_animation();
    });

    m_hover_animation = new QVariantAnimation(this);
    m_hover_animation->setDuration(120);
    m_hover_animation->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_hover_animation, &QVariantAnimation::valueChanged, this, [this](QVariant const& value) {
        m_hover_progress = value.toReal();
        update();
    });
    connect(m_hover_animation, &QVariantAnimation::finished, this, [this] {
        if (m_hover_progress <= 0.0)
            m_hover_animation_tab_index = -1;
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

    if (m_tab_layout == TabLayout::Horizontal) {
        setShape(QTabBar::RoundedNorth);
        setMinimumSize({ 0, HORIZONTAL_TAB_HEIGHT });
        setMaximumWidth(QWIDGETSIZE_MAX);
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        setUsesScrollButtons(false);
        set_vertical_scroll_offset(0);
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

void TabBar::refresh_tab_layout()
{
    set_vertical_scroll_offset(m_vertical_scroll_offset);
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
    m_fallback_tab_icon = create_chrome_icon(ChromeIcon::Globe, palette());
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
        return { vertical_tab_width(m_available_width, tab_layout()), count() > 0 ? (2 * VERTICAL_TAB_FLARE_RADIUS) + VERTICAL_TAB_HEIGHT : 0 };
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
        // The first and last tabs also take room for their flares at the ends of the tab bar.
        auto end_room = (index == 0 ? HORIZONTAL_TAB_END_ROOM : 0) + (index == count - 1 ? HORIZONTAL_TAB_END_ROOM : 0);
        if (index < pinned_count) {
            hint.setWidth(PINNED_TAB_TILE_WIDTH + end_room);
            hint.setHeight(HORIZONTAL_TAB_HEIGHT);
            return hint;
        }

        // The unpinned tabs share whatever width the pinned tiles and the room at the ends leave.
        auto available_width = (m_available_width > 0 ? m_available_width : this->width()) - (pinned_count * PINNED_TAB_TILE_WIDTH) - (2 * HORIZONTAL_TAB_END_ROOM);
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
    set_vertical_scroll_offset(m_vertical_scroll_offset);
    ensure_tab_visible(currentIndex());
    update_tab_button_geometry();
}

void TabBar::tabLayoutChange()
{
    QTabBar::tabLayoutChange();
    set_vertical_scroll_offset(m_vertical_scroll_offset);
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

    // The swirl stops while the tab bar is hidden, so it has to start again for tabs still loading when it is shown.
    if (event->type() == QEvent::Show)
        update_loading_swirl_animation();

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
    }

    return QTabBar::event(event);
}

void TabBar::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    auto dark = ChromeStyle::is_dark(palette());
    auto text_color = ChromeStyle::chrome_text(palette());
    auto is_vertical = tab_layout() != TabLayout::Horizontal;
    auto is_collapsed_vertical = tab_layout() == TabLayout::VerticalCollapsed;
    auto dragged_index = dragged_tab_index();
    auto hover_progress_of = [&](int index) {
        return index == m_hover_animation_tab_index ? m_hover_progress : (index == m_hovered_tab_index ? 1.0 : 0.0);
    };
    auto region = pinned_region();
    auto pinned_count = is_vertical ? region.count : pinned_tab_count();
    // While tabs make way for a pinned tab dragged in from another window, the pinned tabs can take up another row.
    auto pinned_region_growth = qRound(current_pinned_region_growth());
    auto fixed_height = region.scrolls ? 0 : region.height + pinned_region_growth;
    // NB: Without pinned tabs in the way, the unpinned tabs also use the room above them.
    auto unpinned_clip_top = fixed_height > 0 ? VERTICAL_TAB_FLARE_RADIUS + fixed_height : 0;
    QRect unpinned_clip { 0, unpinned_clip_top, width(), max(0, height() - unpinned_clip_top) };

    // Titles are shaped once and kept for as long as some tab shows them, since resizing the window repaints the tab
    // bar on every step without changing any title.
    auto title_font = ChromeStyle::chrome_title_font(font());
    if (m_tab_title_texts_font != title_font) {
        m_tab_title_texts.clear();
        m_tab_title_texts_font = title_font;
    }
    QHash<QString, QStaticText> title_texts;
    auto title_line_height = QFontMetricsF(title_font).height();

    for (int index = 0; index < count(); ++index) {
        // The tab being dragged follows the cursor as the drag's image, leaving a gap where it would be dropped.
        if (index == dragged_index)
            continue;

        // Unpinned tabs scroll under the pinned ones, so they never paint over them. The clips that each tab's sheen
        // and title set later intersect with this one, rather than replacing it.
        if (is_vertical) {
            if (index >= pinned_count)
                painter.setClipRect(unpinned_clip);
            else
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
        auto hover_progress = hover_progress_of(index);
        bool is_hovered = hover_progress > 0.0;
        // A pinned tab is a tile that shows only its favicon, except where the pinned tabs keep their rows.
        auto is_pinned_tile = index < pinned_count && pinned_tabs_are_tiles();

        QRectF shape_rect;
        if (is_collapsed_vertical)
            shape_rect = collapsed_vertical_tab_shape_rect(QRectF(tab_rect));
        else if (!is_vertical)
            shape_rect = horizontal_tab_card_shape_rect(QRectF(tab_rect), tab_bar_edges(index, count()));
        else
            shape_rect = tab_card_shape_rect(QRectF(tab_rect));
        QPainterPath tab_path;
        if (!is_vertical && is_selected)
            tab_path = horizontal_selected_tab_path(QRectF(shape_rect.left(), shape_rect.top(), shape_rect.width(), tab_rect.bottom() + 1 - shape_rect.top()));
        else if (is_selected && !is_pinned_tile)
            tab_path = vertical_selected_tab_path(shape_rect, vertical_tabs_are_on_right() ? 0.0 : static_cast<qreal>(width()));
        else
            tab_path.addRoundedRect(shape_rect, shape_rect.height() / 2.0, shape_rect.height() / 2.0);

        // Horizontal tabs are divided by lines, except around the selected tab and the hovered one.
        if (!is_vertical) {
            auto next_index = index + 1;
            auto next_is_selected = next_index == currentIndex();
            auto separator_opacity = 1.0 - max(hover_progress, hover_progress_of(next_index));
            if (!is_selected && !next_is_selected && separator_opacity > 0.0) {
                auto separator_center_y = shape_rect.center().y();
                // NB: The separator sits as far from the card as the edge between two tabs, which is also where it sits after
                //     the last tab, whose room for its flare lies beyond. This keeps it on whole device pixels at a pixel
                //     ratio of 2.
                auto separator_x = shape_rect.right() + HORIZONTAL_TAB_CARD_SHAPE_HORIZONTAL_INSET - 1;
                QRectF separator_rect { separator_x, separator_center_y - (HORIZONTAL_TAB_SEPARATOR_HEIGHT / 2.0), HORIZONTAL_TAB_SEPARATOR_WIDTH, HORIZONTAL_TAB_SEPARATOR_HEIGHT };
                painter.save();
                painter.setOpacity(separator_opacity);
                painter.fillRect(separator_rect, horizontal_tab_separator(*this));
                painter.restore();
            }
        }

        if (!is_vertical && is_selected) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(ChromeStyle::chrome_active_tab_surface(palette()));
            painter.drawPath(tab_path);
        } else if (is_selected) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(ChromeStyle::chrome_active_tab_surface(palette()));
            painter.drawPath(tab_path);
        } else if (is_hovered) {
            painter.setBrush(tab_hover_surface(palette(), hover_progress));
            painter.setPen(Qt::NoPen);
            painter.drawPath(tab_path);
        }

        if (is_tab_loading(index))
            ChromeStyle::paint_loading_swirl(painter, tab_path);
        if (auto progress = load_sheen_progress(index); progress.has_value())
            paint_load_sheen(painter, palette(), tab_path, tab_path.boundingRect(), *progress);

        auto icon = tabIcon(index);
        auto paint_icon = [&](QRect const& icon_rect) {
            if (!icon.isNull()) {
                icon.paint(&painter, icon_rect);
                return;
            }
            // NB: The fallback globe is drawn on a chrome icon's larger canvas, with room around it, so it is painted at
            //     that size to be about as large as a favicon.
            auto overhang = (CHROME_ICON_CANVAS_SIZE - TAB_ICON_SIZE) / 2;
            m_fallback_tab_icon.paint(&painter, icon_rect.adjusted(-overhang, -overhang, overhang, overhang));
        };

        if (is_collapsed_vertical || is_pinned_tile) {
            // The audio button takes the favicon's place on a pinned tile.
            if (is_pinned_tile && tabButton(index, QTabBar::LeftSide))
                continue;

            // NB: A QRect's center is rounded down, so add 1 to center the icon in the tab.
            auto icon_center = is_vertical ? QPointF(tab_rect.center() + QPoint(1, 1)) : shape_rect.center();
            QRect icon_rect {
                qRound(icon_center.x()) - (TAB_ICON_SIZE / 2),
                qRound(icon_center.y()) - (TAB_ICON_SIZE / 2),
                TAB_ICON_SIZE,
                TAB_ICON_SIZE,
            };
            if (!is_selected)
                painter.setOpacity(is_hovered ? 0.92 : (dark ? 0.88 : 0.84));
            paint_icon(icon_rect);
            painter.setOpacity(1.0);
            continue;
        }

        auto contents_rect = shape_rect.toAlignedRect().adjusted(TAB_CONTENT_HORIZONTAL_INSET, 0, -TAB_CONTENT_HORIZONTAL_INSET, 0);
        if (auto* left_button = tabButton(index, QTabBar::LeftSide); left_button && left_button->isVisible())
            contents_rect.setLeft(max(contents_rect.left(), left_button->geometry().right() + 6));
        if (auto* right_button = tabButton(index, QTabBar::RightSide); right_button && right_button->isVisible())
            contents_rect.setRight(min(contents_rect.right(), right_button->geometry().left() - 6));

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
            if (!is_selected)
                painter.setOpacity(is_hovered ? 0.92 : (dark ? 0.88 : 0.84));
            paint_icon(icon_rect);
            painter.setOpacity(1.0);
            continue;
        }

        QRect icon_rect {
            contents_rect.left(),
            contents_rect.top() + ((contents_rect.height() - TAB_ICON_SIZE) / 2),
            TAB_ICON_SIZE,
            TAB_ICON_SIZE,
        };
        if (!is_selected)
            painter.setOpacity(is_hovered ? 0.92 : (dark ? 0.88 : 0.84));
        paint_icon(icon_rect);
        painter.setOpacity(1.0);
        contents_rect.setLeft(icon_rect.right() + 8);

        painter.setFont(title_font);

        auto title = tabText(index);
        auto title_text = m_tab_title_texts.value(title);
        if (title_text.text().isEmpty() && !title.isEmpty()) {
            auto single_line_title = title;
            single_line_title.replace(QLatin1Char('\n'), QLatin1Char(' '));
            title_text.setText(single_line_title);
            title_text.setTextFormat(Qt::PlainText);
            title_text.prepare(QTransform(), title_font);
        }
        title_texts.insert(title, title_text);

        auto tab_text_color = text_color;
        if (!is_selected)
            tab_text_color.setAlpha(is_hovered ? (dark ? 236 : 228) : (dark ? 226 : 216));

        // NB: A static text's height includes the font's leading, but a title is centered on its ascent and descent.
        auto title_size = title_text.size();
        QPointF title_position(contents_rect.left(), contents_rect.top() + ((contents_rect.height() - title_line_height) / 2));

        painter.save();
        painter.setPen(tab_text_color);

        // A title too long for its tab fades out toward the tab's end, rather than being cut short with an ellipsis.
        // NB: Compare in fractional pixels, so a title that just fits is not taken for one that does not.
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
    }

    m_tab_title_texts = AK::move(title_texts);

    painter.setClipping(false);
    if (is_vertical && (region.count > 0 || pinned_region_growth > 0)) {
        auto separator = ChromeStyle::chrome_border(palette());
        separator.setAlpha(dark ? 40 : 32);
        painter.setPen(separator);
        auto separator_y = VERTICAL_TAB_FLARE_RADIUS + region.height + pinned_region_growth - (PINNED_TABS_SEPARATOR_HEIGHT / 2) - (region.scrolls ? m_vertical_scroll_offset : 0);
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

    if (tab_layout() != TabLayout::Horizontal) {
        if (pressed_tab >= 0 && event->button() == Qt::LeftButton) {
            setCurrentIndex(pressed_tab);
        } else if (event->button() == Qt::LeftButton && start_window_move()) {
            event->accept();
            return;
        }
    } else {
        if (pressed_tab < 0 && event->button() == Qt::LeftButton && start_window_move()) {
            event->accept();
            return;
        }
        QTabBar::mousePressEvent(event);
    }

    if (m_pressed_tab)
        event->accept();
    else
        event->ignore();
}

void TabBar::mouseMoveEvent(QMouseEvent* event)
{
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
        } else if ((event->pos() - m_drag_start_position).manhattanLength() >= QApplication::startDragDistance()) {
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

void TabBar::leaveEvent(QEvent* event)
{
    hide_tab_preview();
    set_hovered_tab_index(-1);
    QTabBar::leaveEvent(event);
}

void TabBar::mouseReleaseEvent(QMouseEvent* event)
{
    auto pressed_tab = m_pressed_tab;
    m_pressed_tab = nullptr;

    if (event->button() == Qt::MiddleButton && pressed_tab && m_tab_widget) {
        if (auto index = m_tab_widget->index_of(pressed_tab); index >= 0 && tab_index_at(event->pos()) == index) {
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
    if (tab_layout() == TabLayout::Horizontal || max_vertical_scroll_offset() <= 0) {
        QTabBar::wheelEvent(event);
        return;
    }

    auto old_offset = m_vertical_scroll_offset;
    auto pixel_delta = event->pixelDelta().y();
    auto angle_delta = event->angleDelta().y();

    if (pixel_delta != 0) {
        set_vertical_scroll_offset(m_vertical_scroll_offset - pixel_delta);
    } else if (angle_delta != 0) {
        auto scroll_delta = angle_delta * VERTICAL_TAB_HEIGHT / 120;
        if (scroll_delta == 0)
            scroll_delta = angle_delta > 0 ? 1 : -1;
        set_vertical_scroll_offset(m_vertical_scroll_offset - scroll_delta);
    }

    if (m_vertical_scroll_offset != old_offset)
        event->accept();
    else
        event->ignore();
}

Optional<int> TabBar::dragged_tab_index() const
{
    if (!m_tab_widget || s_active_tab_drag_source != m_tab_widget || !s_active_tab_dragged_tab)
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
    auto first_tab_rect = tabRect(0);
    auto strip_width = tabRect(count() - 1).right() + 1 - first_tab_rect.left() - (2 * HORIZONTAL_TAB_END_ROOM);
    auto unpinned_width = unpinned_count > 0 ? max(0.0, static_cast<qreal>(strip_width - (pinned_count * PINNED_TAB_TILE_WIDTH)) / unpinned_count) : 0.0;

    auto left = first_tab_rect.left() + HORIZONTAL_TAB_END_ROOM + (min(position, pinned_count) * PINNED_TAB_TILE_WIDTH) + (max(0, position - pinned_count) * unpinned_width);
    auto width = position < pinned_count ? PINNED_TAB_TILE_WIDTH : unpinned_width;
    // NB: The first and last places also take room for their flares at the ends of the tab bar.
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
QRect TabBar::shifted_tab_rect(int index) const
{
    auto tab_rect = visual_tab_rect(index);
    auto shift = current_tab_shift(index);
    return {
        qRound(tab_rect.left() + shift.x_offset),
        qRound(tab_rect.top() + shift.y_offset),
        qRound(tab_rect.width() + shift.width_delta),
        qRound(tab_rect.height() + shift.height_delta),
    };
}

QPixmap TabBar::render_tab_drag_pixmap(int index) const
{
    auto tab_rect = visual_tab_rect(index);
    QPixmap pixmap(tab_rect.size() * devicePixelRatioF());
    pixmap.setDevicePixelRatio(devicePixelRatioF());
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setOpacity(0.75);
    const_cast<TabBar&>(*this).render(&painter, QPoint(), QRegion(tab_rect), QWidget::DrawChildren);
    return pixmap;
}

void TabBar::start_tab_drag(int index)
{
    hide_tab_preview();

    if (!m_tab_widget)
        return;

    auto* tab = m_tab_widget->tab(index);
    if (!tab)
        return;

    QPointer<Tab> dragged_tab = tab;
    auto* drag = new QDrag(this);
    auto* mime_data = new QMimeData;
    mime_data->setData(LADYBIRD_TAB_MIME_TYPE, QByteArray {});
    drag->setMimeData(mime_data);
    drag->setPixmap(render_tab_drag_pixmap(index));
    drag->setHotSpot(m_position_in_selected_tab_while_dragging);

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

    static auto const is_wayland = QGuiApplication::platformName().startsWith("wayland");
    auto was_canceled = cancellation_filter.was_canceled() || (is_wayland && action == Qt::IgnoreAction);

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

void TabBar::start_hover_animation(int tab_index, qreal target_progress)
{
    if (!m_hover_animation)
        return;

    if (tab_index < 0) {
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

void TabBar::play_load_sheen(Tab& tab)
{
    m_load_sheens.remove_all_matching([&](auto const& sheen) {
        if (sheen.tab != &tab)
            return false;
        sheen.animation->deleteLater();
        return true;
    });

    auto* animation = new QVariantAnimation(this);
    animation->setDuration(LOAD_SHEEN_DURATION_MS);
    animation->setEasingCurve(QEasingCurve::InOutSine);
    animation->setStartValue(0.0);
    animation->setEndValue(1.0);
    connect(animation, &QVariantAnimation::valueChanged, this, [this] { update(); });
    connect(animation, &QVariantAnimation::finished, this, [this, animation] {
        m_load_sheens.remove_first_matching([&](auto const& sheen) { return sheen.animation == animation; });
        animation->deleteLater();
        update();
    });
    m_load_sheens.append({ &tab, animation });
    animation->start();
}

bool TabBar::is_tab_loading(int index) const
{
    if (!m_tab_widget || index < 0 || index >= count())
        return false;
    auto* tab = m_tab_widget->tab(index);
    return tab && tab->is_loading();
}

// The swirl only needs repainting while a tab is loading and the tab bar can be seen.
void TabBar::update_loading_swirl_animation()
{
    auto any_tab_loading = false;
    for (int index = 0; index < count() && !any_tab_loading; ++index)
        any_tab_loading = is_tab_loading(index);

    auto should_run = any_tab_loading && isVisible();
    if (should_run && m_loading_swirl_animation->state() != QAbstractAnimation::Running)
        m_loading_swirl_animation->start();
    else if (!should_run && m_loading_swirl_animation->state() != QAbstractAnimation::Stopped)
        m_loading_swirl_animation->stop();
    update();
}

Optional<qreal> TabBar::load_sheen_progress(int index) const
{
    if (m_load_sheens.is_empty() || !m_tab_widget)
        return {};
    // NB: A sheen whose tab was closed has a null tab, which must not match a missing one here.
    auto* tab = m_tab_widget->tab(index);
    if (!tab)
        return {};
    for (auto const& sheen : m_load_sheens) {
        if (sheen.tab == tab)
            return sheen.animation->currentValue().toReal();
    }
    return {};
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
            tab_rect.bottom() + TAB_PREVIEW_CARD_GAP - TAB_PREVIEW_SHADOW_MARGIN,
        });

        if (position.y() + popup_size.height() > available_geometry.bottom())
            position.setY(mapToGlobal(QPoint { 0, tab_rect.top() }).y() - popup_size.height() - TAB_PREVIEW_CARD_GAP + TAB_PREVIEW_SHADOW_MARGIN);
    } else {
        auto place_popup_to_right = !vertical_tabs_are_on_right();
        // The popup sits beside the tab bar rather than the tab, so it never covers the other pinned tiles in a row.
        auto bar_rect = rect();
        position = mapToGlobal(QPoint {
            place_popup_to_right
                ? bar_rect.right() + TAB_PREVIEW_CARD_GAP - TAB_PREVIEW_SHADOW_MARGIN
                : bar_rect.left() - popup_size.width() - TAB_PREVIEW_CARD_GAP + TAB_PREVIEW_SHADOW_MARGIN,
            tab_rect.center().y() - popup_size.height() / 2,
        });

        if (place_popup_to_right && position.x() + popup_size.width() > available_geometry.right())
            position.setX(mapToGlobal(QPoint { bar_rect.left(), 0 }).x() - popup_size.width() - TAB_PREVIEW_CARD_GAP + TAB_PREVIEW_SHADOW_MARGIN);
        else if (!place_popup_to_right && position.x() < available_geometry.left())
            position.setX(mapToGlobal(QPoint { bar_rect.right(), 0 }).x() + TAB_PREVIEW_CARD_GAP - TAB_PREVIEW_SHADOW_MARGIN);
    }

    auto minimum_x = available_geometry.left() + TAB_PREVIEW_CARD_GAP - TAB_PREVIEW_SHADOW_MARGIN;
    auto maximum_x = available_geometry.right() - popup_size.width() - TAB_PREVIEW_CARD_GAP + TAB_PREVIEW_SHADOW_MARGIN;
    if (minimum_x <= maximum_x)
        position.setX(clamp(position.x(), minimum_x, maximum_x));

    auto minimum_y = available_geometry.top() + TAB_PREVIEW_CARD_GAP - TAB_PREVIEW_SHADOW_MARGIN;
    auto maximum_y = available_geometry.bottom() - popup_size.height() - TAB_PREVIEW_CARD_GAP + TAB_PREVIEW_SHADOW_MARGIN;
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
        region.columns = max(1, bar_width / PINNED_TAB_TILE_WIDTH);
        region.tile_width = bar_width / region.columns;
    }

    auto rows = (region.count + region.columns - 1) / region.columns;
    region.height = (rows * VERTICAL_TAB_HEIGHT) + PINNED_TABS_SEPARATOR_HEIGHT;
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
        return tabRect(index);

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
        auto top = VERTICAL_TAB_FLARE_RADIUS + (row * VERTICAL_TAB_HEIGHT) - (region.scrolls ? m_vertical_scroll_offset : 0);
        return { vertical_tabs_left() + (column * region.tile_width), top, region.tile_width, VERTICAL_TAB_HEIGHT };
    }

    auto unpinned_index = index - region.count;
    return { vertical_tabs_left(), VERTICAL_TAB_FLARE_RADIUS + region.height + (unpinned_index * VERTICAL_TAB_HEIGHT) - m_vertical_scroll_offset, vertical_bar_width(), VERTICAL_TAB_HEIGHT };
}

int TabBar::tab_index_at(QPoint const& position) const
{
    if (tab_layout() == TabLayout::Horizontal)
        return tabAt(position);

    if (position.x() < 0 || position.x() >= width() || position.y() < 0 || position.y() >= height())
        return -1;

    auto region = pinned_region();
    auto y = position.y() - VERTICAL_TAB_FLARE_RADIUS;
    if (y < 0)
        return -1;
    auto pinned_y = y + (region.scrolls ? m_vertical_scroll_offset : 0);
    if (pinned_y < region.height) {
        if (pinned_y >= region.height - PINNED_TABS_SEPARATOR_HEIGHT)
            return -1;
        auto x = position.x() - vertical_tabs_left();
        if (x < 0)
            return -1;
        auto row = pinned_y / VERTICAL_TAB_HEIGHT;
        auto column = min(x / region.tile_width, region.columns - 1);
        auto index = (row * region.columns) + column;
        return index < region.count ? index : -1;
    }

    auto index = region.count + ((y - region.height + m_vertical_scroll_offset) / VERTICAL_TAB_HEIGHT);
    if (index >= count())
        return -1;

    if (!visual_tab_rect(index).contains(position))
        return -1;

    return index;
}

QSize TabBar::vertical_size_hint(int tab_count) const
{
    auto region = pinned_region();
    return { vertical_tab_width(m_available_width, tab_layout()), (2 * VERTICAL_TAB_FLARE_RADIUS) + region.height + (max(0, tab_count - region.count) * VERTICAL_TAB_HEIGHT) };
}

bool TabBar::vertical_tabs_are_on_right() const
{
    return m_vertical_tabs_position == WebView::VerticalTabsPosition::Right;
}

int TabBar::max_vertical_scroll_offset() const
{
    if (tab_layout() == TabLayout::Horizontal)
        return 0;
    auto region = pinned_region();
    return max(0, (2 * VERTICAL_TAB_FLARE_RADIUS) + region.height + ((count() - region.count) * VERTICAL_TAB_HEIGHT) - height());
}

void TabBar::set_vertical_scroll_offset(int offset)
{
    auto clamped_offset = max(0, min(offset, max_vertical_scroll_offset()));
    if (m_vertical_scroll_offset == clamped_offset)
        return;

    m_vertical_scroll_offset = clamped_offset;
    update_tab_button_geometry();
    update();
}

void TabBar::ensure_tab_visible(int index)
{
    if (tab_layout() == TabLayout::Horizontal || index < 0 || index >= count())
        return;

    // A pinned tab that stays in place is always in view.
    auto region = pinned_region();
    if (index < region.count && !region.scrolls)
        return;

    // Where the tab sits in the part of the bar that scrolls.
    auto tab_top = index < region.count
        ? (index / region.columns) * VERTICAL_TAB_HEIGHT
        : region.height + ((index - region.count) * VERTICAL_TAB_HEIGHT) - region.fixed_height();
    auto tab_bottom = tab_top + VERTICAL_TAB_HEIGHT;
    auto visible_height = height() - (2 * VERTICAL_TAB_FLARE_RADIUS) - region.fixed_height();

    if (tab_top < m_vertical_scroll_offset)
        set_vertical_scroll_offset(tab_top);
    else if (tab_bottom > m_vertical_scroll_offset + visible_height)
        set_vertical_scroll_offset(tab_bottom - visible_height);
}

void TabBar::set_hovered_tab_index(int index)
{
    if (m_hovered_tab_index == index)
        return;

    auto previous_hovered_tab = m_hovered_tab_index;
    m_hovered_tab_index = index;
    update_tab_button_geometry();
    start_hover_animation(index >= 0 ? index : previous_hovered_tab, index >= 0 ? 1.0 : 0.0);
    schedule_tab_preview(index);
    update();
}

void TabBar::update_tab_button_geometry()
{
    auto set_button_collapsed_overlay = [](QWidget* button, bool enabled) {
        if (auto* tab_bar_button = qobject_cast<TabBarButton*>(button))
            tab_bar_button->set_collapsed_vertical_overlay(enabled);
    };

    auto prepare_button = [&](QWidget* button, bool collapsed_overlay) {
        if (!button)
            return;
        button->installEventFilter(this);
        set_button_collapsed_overlay(button, collapsed_overlay);
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
        for (int index = 0; index < count(); ++index) {
            auto is_dragged = index == dragged_index;
            auto tab_rect = shifted_tab_rect(index);
            auto shape_rect = horizontal_tab_card_shape_rect(QRectF(tab_rect), tab_bar_edges(index, count()));
            auto* left_button = tabButton(index, QTabBar::LeftSide);
            prepare_button(left_button, false);

            if (index < pinned_count) {
                if (!left_button)
                    continue;
                center_button_in(left_button, shape_rect);
                if (left_button->isVisible() == is_dragged)
                    left_button->setVisible(!is_dragged);
                continue;
            }

            // NB: The style puts a tab's left button 1pt inside the tab's card, where its icon looks flush against the
            //     card's edge. Give it 3pt.
            if (left_button) {
                QPoint position { qRound(shape_rect.left()) + 3, button_y_in(left_button, shape_rect) };
                if (left_button->pos() != position)
                    left_button->move(position);
                if (left_button->isVisible() == is_dragged)
                    left_button->setVisible(!is_dragged);
            }
            auto* close_button = tabButton(index, QTabBar::RightSide);
            prepare_button(close_button, false);
            if (!close_button)
                continue;
            // The close button's circle shares its center with the round end of the card.
            auto close_button_center_x = shape_rect.right() - (shape_rect.height() / 2.0);
            QPoint close_button_position { qRound(close_button_center_x - (close_button->width() / 2.0)), button_y_in(close_button, shape_rect) };
            if (close_button->pos() != close_button_position)
                close_button->move(close_button_position);
            auto minimum_width = index == currentIndex() ? HORIZONTAL_ACTIVE_TAB_CLOSE_BUTTON_MIN_WIDTH : HORIZONTAL_TAB_CLOSE_BUTTON_MIN_WIDTH;
            auto show_close_button = !is_dragged && tab_rect.width() >= minimum_width;
            if (close_button->isVisible() != show_close_button)
                close_button->setVisible(show_close_button);
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
        prepare_button(button, false);

        auto button_size = button->size();
        if (button_size.isEmpty())
            button_size = button->sizeHint();
        if (button_size.isEmpty())
            return;

        // A button's circle shares its center with the round end of the card it sits in.
        // NB: A QRect's right() is its last column, one short of its right edge.
        auto end_radius = shape_rect.height() / 2;
        auto x = position == QTabBar::RightSide ? shape_rect.right() + 1 - end_radius - (button_size.width() / 2) : shape_rect.left() + end_radius - (button_size.width() / 2);
        auto icon_left = shape_rect.left() + TAB_CONTENT_HORIZONTAL_INSET;
        if (position == QTabBar::RightSide) {
            if (auto* left_button = tabButton(index, QTabBar::LeftSide); left_button && left_button->isVisible())
                icon_left = max(icon_left, left_button->geometry().right() + 6);
        }

        // The expanded layout is also used at narrow widths during hover expansion. Keep buttons from covering the
        // favicon while the sidebar grows.
        auto has_room_for_icon = position == QTabBar::RightSide
            ? x - 6 >= icon_left + TAB_ICON_SIZE
            : x + button_size.width() + 6 + TAB_ICON_SIZE <= shape_rect.right() - TAB_CONTENT_HORIZONTAL_INSET;

        // NB: Move a button before showing it. Shown where it was, under the pointer, it would take a hover that it
        //     keeps at its new place until the pointer moves.
        auto did_update_button = false;
        auto y = shape_rect.top() + ((shape_rect.height() - button_size.height()) / 2);
        QRect button_geometry { { x, y }, button_size };
        if (button->geometry() != button_geometry) {
            button->setGeometry(button_geometry);
            did_update_button = true;
        }

        auto should_be_visible = index != dragged_index && has_room_for_icon
            && (position != QTabBar::RightSide || index == currentIndex() || index == m_hovered_tab_index);
        if (button->isVisible() != should_be_visible) {
            button->setVisible(should_be_visible);
            did_update_button = true;
        }

        if (did_update_button)
            button->raise();
    };

    auto place_collapsed_button = [&](int index, QTabBar::ButtonPosition position, QRect tab_rect, QRect shape_rect) {
        auto* button = tabButton(index, position);
        if (!button)
            return;
        prepare_button(button, true);

        auto button_size = button->size();
        if (button_size.isEmpty())
            button_size = button->sizeHint();
        if (button_size.isEmpty())
            return;

        QPoint button_position;
        if (position == QTabBar::RightSide) {
            button_position = {
                max(tab_rect.left(), shape_rect.left() - 5),
                max(tab_rect.top(), shape_rect.top() - 5),
            };
        } else {
            auto x = min(tab_rect.right() - button_size.width() + 1, shape_rect.right() - button_size.width() + 5);
            auto y = min(tab_rect.bottom() - button_size.height() + 1, shape_rect.bottom() - button_size.height() + 5);
            button_position = { x, y };
        }

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
            place_collapsed_button(index, QTabBar::LeftSide, tab_rect, shape_rect);
            place_collapsed_button(index, QTabBar::RightSide, tab_rect, shape_rect);
        } else if (index < region.count && pinned_tabs_are_tiles()) {
            if (auto* button = tabButton(index, QTabBar::LeftSide)) {
                prepare_button(button, false);
                center_button_in(button, tab_rect);
                if (button->isVisible() != (index != dragged_index))
                    button->setVisible(index != dragged_index);
            }
        } else {
            auto shape_rect = tab_card_shape_rect(tab_rect);
            place_expanded_button(index, QTabBar::LeftSide, shape_rect);
            place_expanded_button(index, QTabBar::RightSide, shape_rect);
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

TabWidget::TabWidget(QWidget* parent)
    : QWidget(parent)
{
    if (auto* top_level_window = window(); top_level_window != this)
        top_level_window->installEventFilter(this);

    m_vertical_tabs_expanded_width = Application::settings().tab_settings().vertical_tabs_expanded_width.value_or(VERTICAL_TABS_DEFAULT_EXPANDED_WIDTH);
    m_vertical_tabs_expanded_width = clamp_vertical_tabs_expanded_width(m_vertical_tabs_expanded_width);

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
    m_page_column_layout = new QVBoxLayout(m_page_column);
    m_page_column_layout->setSpacing(0);
    m_page_column_layout->setContentsMargins(0, 0, 0, 0);

    m_new_tab_button = new NewTabButton(*m_tab_bar, this);
    m_new_tab_button->setObjectName("LadybirdNewTabButton");
    m_new_tab_button->setIconSize(QSize(18, 18));
    m_new_tab_button->setFixedSize(32, 32);
    m_new_tab_button->setFocusPolicy(Qt::NoFocus);
    m_new_tab_button->setToolTip("New Tab");
    m_new_tab_button->installEventFilter(this);

    if constexpr (use_native_macos_window_controls()) {
        // NB: The first tab's room for its flare takes the place of the gap after the window controls.
        m_window_controls = create_window_controls_spacer(*this, "LadybirdTabStripWindowControls", { NATIVE_MACOS_WINDOW_CONTROLS_WIDTH + max(0, NATIVE_MACOS_WINDOW_CONTROLS_GAP - HORIZONTAL_TAB_END_ROOM), HORIZONTAL_TAB_HEIGHT });
    } else {
        auto window_control_buttons = create_window_control_buttons(*this, "LadybirdTabStripWindowControls", { 18, 18 }, { 40, 40 });
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
    m_vertical_tabs_toggle_button->setIconSize({ 20, 20 });
    m_vertical_tabs_toggle_button->setFixedSize(VERTICAL_TABS_TOGGLE_BUTTON_SIZE, VERTICAL_TABS_TOGGLE_BUTTON_SIZE);

    m_vertical_tabs_content_separator = new QWidget(this);
    m_vertical_tabs_content_separator->setObjectName("LadybirdVerticalTabsContentSeparator");
    m_vertical_tabs_content_separator->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_vertical_tabs_content_separator->hide();

    m_vertical_tabs_reserved_space = new QWidget(m_vertical_tabs_content);
    m_vertical_tabs_reserved_space->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);

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
        // A tab that hasn't loaded gets the filters for its view and bookmarks bar once it has them.
        auto install_event_filters = [this, widget] {
            widget->view().installEventFilter(this);
            widget->bookmarks_bar().installEventFilter(this);
        };
        connect(widget, &Tab::loaded, this, install_event_filters, Qt::SingleShotConnection);
    }

    m_stacked_widget->insertWidget(index, widget);
    m_tab_bar->insertTab(index, label);
    widget->set_toolbar_container_in_tab_layout(false);
    m_toolbar_container->insertWidget(index, widget->toolbar_container());

    widget->set_vertical_tabs_enabled(m_vertical_tabs_enabled);
#ifdef AK_OS_MACOS
    update_web_view_frame(*widget);
#endif

    update_toolbar_placement();
    update_tab_layout();
    update_tab_button_visibility();

    // A tab moved here from another window can already be loading.
    m_tab_bar->update_loading_swirl_animation();
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
    if (m_vertical_tabs_expanded == expanded)
        return;

    m_vertical_tabs_expanded = expanded;
    set_vertical_tabs_hover_expanded(false);
    rebuild_layout();
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
        // The tab strip and the sidebar are lighter while the window is inactive.
        auto inactive = type == QEvent::WindowDeactivate;
        set_dynamic_property_if_needed(*m_tab_bar_row, WINDOW_INACTIVE_PROPERTY, inactive);
        set_dynamic_property_if_needed(*m_vertical_tab_bar_column, WINDOW_INACTIVE_PROPERTY, inactive);
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

    if (watched == m_vertical_tab_bar_column && event->type() == QEvent::Paint && !m_vertical_tabs_expanded) {
        paint_collapsed_vertical_tabs_column();
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

    if (watched == m_toolbar_container && event->type() == QEvent::Resize)
        update_tab_layout();

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

    auto* drag_area = tab_drag_area_widget();
    auto position_in_drag_area = drag_area->mapFrom(target, event->position().toPoint());
    if (!drag_area->rect().contains(position_in_drag_area)) {
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

int TabWidget::vertical_tabs_column_top_margin() const
{
    // NB: The open sidebar's top row is level with the toolbar, at the top of the window.
    return m_vertical_tabs_expanded ? 0 : VERTICAL_TABS_TOP_MARGIN;
}

void TabWidget::update_toolbar_placement()
{
    auto use_full_width_toolbar = current_tab_layout() != TabLayout::Horizontal;

    for (int index = 0; index < m_stacked_widget->count(); ++index) {
        auto* current_tab = tab(index);
        auto* toolbar = current_tab->toolbar_container();
        set_dynamic_property_if_needed(*toolbar, FULL_WIDTH_TOOLBAR_PROPERTY, use_full_width_toolbar);
    }

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
    clear_layout(*m_vertical_tabs_content_layout);

    m_tab_bar->set_vertical_tabs_position(m_vertical_tabs_position);
    m_tab_bar->set_pinned_tabs_in_rows(m_vertical_tabs_hover_expanded && !m_vertical_tabs_expanded);
    m_tab_bar->set_tab_layout(current_tab_layout());
    update_toolbar_placement();

    if (m_tab_bar->tab_layout() != TabLayout::Horizontal) {
        rebuild_layout_for_vertical_tabs();

        // The open sidebar runs the full height of the window, with the toolbar beside it above the page. The collapsed
        // sidebar sits below a toolbar across the whole window.
        QWidget* page = m_stacked_widget;
        if (m_vertical_tabs_expanded) {
            rebuild_page_column();
            page = m_page_column;
            m_page_column->show();
        } else {
            m_main_layout->addWidget(m_toolbar_container);
            m_page_column->hide();
        }

        if (vertical_tabs_are_on_right()) {
            m_vertical_tabs_content_layout->addWidget(page, 1);
            m_vertical_tabs_content_layout->addWidget(m_vertical_tabs_reserved_space);
        } else {
            m_vertical_tabs_content_layout->addWidget(m_vertical_tabs_reserved_space);
            m_vertical_tabs_content_layout->addWidget(page, 1);
        }
        m_main_layout->addWidget(m_vertical_tabs_content, 1);
        m_vertical_tabs_content->show();
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
}

void TabWidget::rebuild_layout_for_horizontal_tabs()
{
    m_tab_bar_row->setMinimumHeight(HORIZONTAL_TAB_STRIP_HEIGHT);
    m_tab_bar_row_layout->setSpacing(0);
    // NB: Without window controls before it, the first tab's room for its flare takes the place of the strip's margin.
    m_tab_bar_row_layout->setContentsMargins(use_native_macos_window_controls() ? 12 : max(0, 12 - HORIZONTAL_TAB_END_ROOM), 2, 4, 0);

    m_new_tab_button->setText({});
    set_dynamic_property_if_needed(*m_new_tab_button, VERTICAL_TABS_EXPANDED_PROPERTY, false);
    set_dynamic_property_if_needed(*m_new_tab_button, VERTICAL_TABS_BUTTON_PROPERTY, true);
    m_new_tab_button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    m_new_tab_button->setFixedSize(HORIZONTAL_TAB_HEIGHT, HORIZONTAL_TAB_HEIGHT);
    m_new_tab_button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    // NB: The tabs sit on the bottom of the strip, so the selected tab meets the toolbar however tall the strip is.
    // NB: The new tab button overlaps the tab bar, which a layout cannot do, so the row only leaves room for the rest of
    //     it, and place_horizontal_new_tab_button() puts it in place.
    m_new_tab_button->setParent(m_tab_bar_row);
    m_new_tab_button->show();
    auto new_tab_button_room = HORIZONTAL_TAB_HEIGHT - HORIZONTAL_NEW_TAB_BUTTON_OVERLAP;
    if constexpr (use_native_macos_window_controls()) {
        m_tab_bar_row_layout->addWidget(m_window_controls, 0, Qt::AlignVCenter);
        m_tab_bar_row_layout->addWidget(m_tab_bar, 0, Qt::AlignBottom);
        m_tab_bar_row_layout->addSpacing(new_tab_button_room);
        m_tab_bar_row_layout->addStretch(1);
    } else {
        m_tab_bar_row_layout->addWidget(m_tab_bar, 0, Qt::AlignBottom);
        m_tab_bar_row_layout->addSpacing(new_tab_button_room);
        m_tab_bar_row_layout->addStretch(1);
        m_tab_bar_row_layout->addWidget(m_window_controls);
    }
    place_horizontal_new_tab_button();
}

void TabWidget::place_horizontal_new_tab_button()
{
    if (m_tab_bar->tab_layout() != TabLayout::Horizontal)
        return;

    auto tab_bar_geometry = m_tab_bar->geometry();
    m_new_tab_button->move(tab_bar_geometry.right() + 1 - HORIZONTAL_NEW_TAB_BUTTON_OVERLAP, tab_bar_geometry.bottom() + 1 - m_new_tab_button->height());
    m_new_tab_button->raise();
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
        // controls when they are on its side, with the sidebar's toggle at the edge toward the page.
        auto page_side_margin = vertical_tabs_side_margin(true);
        top_row_layout->setContentsMargins(vertical_tabs_are_on_right() ? page_side_margin : 0, 0, vertical_tabs_are_on_right() ? 0 : page_side_margin, 0);
        top_row_layout->addStrut(Tab::toolbar_height(should_show_window_controls_in_tab_toolbar()));
        if (vertical_tabs_hold_window_controls())
            top_row_layout->addSpacing(NATIVE_MACOS_WINDOW_CONTROLS_WIDTH);
        if (!vertical_tabs_are_on_right())
            top_row_layout->addStretch(1);
        top_row_layout->addWidget(m_vertical_tabs_toggle_button, 0, Qt::AlignVCenter);
        if (vertical_tabs_are_on_right())
            top_row_layout->addStretch(1);
    } else {
        // The collapsed sidebar's toggle sits above its tabs, centered on the collapsed sidebar, and stays put while
        // the sidebar opens on hover.
        auto collapsed_tab_width = VERTICAL_TABS_COLLAPSED_WIDTH - vertical_tabs_horizontal_margin_width(TabLayout::VerticalCollapsed);
        auto inset = (collapsed_tab_width - VERTICAL_TABS_TOGGLE_BUTTON_SIZE) / 2;
        top_row_layout->setContentsMargins(vertical_tabs_are_on_right() ? 0 : inset, 0, vertical_tabs_are_on_right() ? inset : 0, 0);
        // NB: The tab bar below leaves room above its first tab.
        top_row_layout->addStrut(VERTICAL_TAB_HEIGHT - VERTICAL_TAB_FLARE_RADIUS);
        if (vertical_tabs_are_on_right())
            top_row_layout->addStretch(1);
        top_row_layout->addWidget(m_vertical_tabs_toggle_button, 0, Qt::AlignVCenter);
        if (!vertical_tabs_are_on_right())
            top_row_layout->addStretch(1);
    }
    m_vertical_tab_bar_column_layout->addLayout(top_row_layout);
    if (m_vertical_tabs_expanded)
        m_vertical_tab_bar_column_layout->addSpacing(VERTICAL_TABS_TOP_MARGIN - VERTICAL_TAB_FLARE_RADIUS);

    m_vertical_tab_bar_column_layout->addWidget(m_tab_bar);
    m_vertical_tab_bar_column_layout->addWidget(m_new_tab_button, 0, vertical_tabs_are_on_right() ? Qt::AlignRight : Qt::AlignLeft);
    m_vertical_tab_bar_column_layout->addStretch(1);
}

// The collapsed sidebar sits below the toolbar, which flows around its rounded corner beside the page into the frame
// around the page.
void TabWidget::paint_collapsed_vertical_tabs_column()
{
    QPainter painter(m_vertical_tab_bar_column);
    QStyleOption option;
    option.initFrom(m_vertical_tab_bar_column);
    m_vertical_tab_bar_column->style()->drawPrimitive(QStyle::PE_Widget, &option, &painter, m_vertical_tab_bar_column);

    auto radius = VERTICAL_TABS_COLLAPSED_CORNER_RADIUS;
    auto page_edge = vertical_tabs_are_on_right() ? 0.0 : static_cast<qreal>(m_vertical_tab_bar_column->width());
    auto corner_left = vertical_tabs_are_on_right() ? page_edge : page_edge - radius;

    QPainterPath corner;
    corner.addRect(corner_left, 0, radius, radius);
    QPainterPath inside;
    inside.addEllipse(QPointF(vertical_tabs_are_on_right() ? page_edge + radius : page_edge - radius, radius), radius, radius);

    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillPath(corner.subtracted(inside), ChromeStyle::chrome_background(palette()));
}

void TabWidget::update_vertical_tabs_column_margins()
{
    // NB: The tab bar reaches the page's side of the sidebar, where its selected tab joins the page's frame, so the
    //     column has no margin on that side.
    auto side_margin = vertical_tabs_side_margin(m_tab_bar->tab_layout() != TabLayout::VerticalCollapsed);
    auto left = vertical_tabs_are_on_right() ? 0 : side_margin;
    auto right = vertical_tabs_are_on_right() ? side_margin : 0;
    m_vertical_tab_bar_column_layout->setContentsMargins(left, vertical_tabs_column_top_margin(), right, 8);
}

void TabWidget::rebuild_page_column()
{
    m_page_column_layout->addWidget(m_toolbar_container);
    m_page_column_layout->addWidget(m_stacked_widget, 1);
}

int TabWidget::current_vertical_tabs_width() const
{
    if (m_vertical_tabs_hover_expanded)
        return VERTICAL_TABS_COLLAPSED_WIDTH + qRound((m_vertical_tabs_expanded_width - VERTICAL_TABS_COLLAPSED_WIDTH) * m_vertical_tabs_hover_expand_progress);

    return vertical_tabs_effectively_expanded() ? m_vertical_tabs_expanded_width : VERTICAL_TABS_COLLAPSED_WIDTH;
}

void TabWidget::apply_vertical_tabs_expanded_width(int width)
{
    auto clamped_width = clamp_vertical_tabs_expanded_width(width);
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
    auto show_resize_handle = m_tab_bar_visible && is_vertical && m_vertical_tabs_expanded;
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

void TabWidget::update_vertical_tabs_action_labels()
{
    auto text = vertical_tabs_effectively_expanded() ? "New Tab" : QString {};
    if (m_new_tab_button->text() != text)
        m_new_tab_button->setText(text);
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
    auto tool_button_style = expanded ? Qt::ToolButtonTextBesideIcon : Qt::ToolButtonIconOnly;
    if (m_new_tab_button->toolButtonStyle() != tool_button_style)
        m_new_tab_button->setToolButtonStyle(tool_button_style);
    update_vertical_tabs_action_labels();
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

    auto controls_width = m_new_tab_button->width();
    if (m_window_controls->isVisible())
        controls_width += m_window_controls->width();

    auto available_for_tabs = width() - controls_width - 36;

    // NB: The tab strip has no minimum width from its contents (see the constructor), so it keeps the window wide enough
    //     for its controls with a minimum of its own.
    m_tab_bar_row->setMinimumWidth(controls_width + 36);

    m_tab_bar->set_available_width(available_for_tabs);

    auto pinned_count = m_tab_bar->pinned_tab_count();
    auto tabs_width = (pinned_count * PINNED_TAB_TILE_WIDTH) + ((m_tab_bar->count() - pinned_count) * HORIZONTAL_TAB_MAX_WIDTH) + (2 * HORIZONTAL_TAB_END_ROOM);
    auto tab_bar_width = min(available_for_tabs, tabs_width);
    m_tab_bar->setFixedWidth(max(0, tab_bar_width));
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
    m_vertical_tab_bar_column->setVisible(m_tab_bar_visible && !is_horizontal);
    update_vertical_tabs_content_separator();
#ifdef AK_OS_MACOS
    for (int index = 0; index < m_stacked_widget->count(); ++index)
        update_web_view_frame(*tab(index));
#endif

    auto show_window_controls = m_window_controls_visible
        && m_tab_bar_visible
        && is_horizontal;
    m_window_controls->setVisible(show_window_controls);
}

#ifdef AK_OS_MACOS
void TabWidget::update_web_view_frame(Tab& tab)
{
    Optional<WebView::VerticalTabsPosition> sidebar_position;
    if (m_tab_bar_visible && m_tab_bar->tab_layout() != TabLayout::Horizontal)
        sidebar_position = m_vertical_tabs_position;
    tab.update_web_view_frame(m_tab_bar_visible, sidebar_position);
}
#endif

void TabWidget::recreate_icons()
{
    m_tab_bar->recreate_icons();
    update_vertical_tabs_toggle_icon();
    m_new_tab_button->setIcon(create_chrome_icon(ChromeIcon::NewTab, palette()));
    update_vertical_tabs_action_labels();
    update_window_button_icons();
}

void TabWidget::update_vertical_tabs_toggle_icon()
{
    // NB: The icons are first made before the sidebar and its toggle are.
    if (!m_vertical_tabs_toggle_button)
        return;
    auto icon = vertical_tabs_are_on_right()
        ? (m_vertical_tabs_expanded ? ChromeIcon::VerticalTabBarCollapseRight : ChromeIcon::VerticalTabBarExpandRight)
        : (m_vertical_tabs_expanded ? ChromeIcon::VerticalTabBarCollapse : ChromeIcon::VerticalTabBarExpand);
    m_vertical_tabs_toggle_button->defaultAction()->setIcon(create_chrome_icon(icon, palette()));
}

void TabWidget::update_chrome_style()
{
    if (m_is_updating_chrome_style)
        return;

    m_is_updating_chrome_style = true;
    auto style_sheet = ChromeStyle::tab_widget_style_sheet(palette());
    m_tab_bar_row->setStyleSheet(style_sheet);
    m_vertical_tab_bar_column->setStyleSheet(style_sheet);
    m_vertical_tabs_content_separator->setStyleSheet(style_sheet);
    m_vertical_tabs_resize_handle->setStyleSheet(style_sheet);
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

    auto duration = m_vertical_tabs_hover_expanded
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
        m_minimize_window_button->setIcon(create_chrome_icon(ChromeIcon::WindowMinimize, palette()));
        m_maximize_window_button->setIcon(create_chrome_icon(is_maximized ? ChromeIcon::WindowRestore : ChromeIcon::WindowMaximize, palette()));
        m_maximize_window_button->setToolTip(is_maximized ? "Restore" : "Maximize");
        m_close_window_button->setIcon(create_chrome_icon(ChromeIcon::WindowClose, palette()));
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

void TabBarButton::set_collapsed_vertical_overlay(bool enabled)
{
    if (property(COLLAPSED_VERTICAL_TAB_BUTTON_PROPERTY).toBool() == enabled)
        return;

    setProperty(COLLAPSED_VERTICAL_TAB_BUTTON_PROPERTY, enabled);

    auto button_size = enabled ? COLLAPSED_VERTICAL_TAB_BUTTON_SIZE : TAB_BUTTON_SIZE;
    setFixedSize(button_size, button_size);
    update_icon_size();
    style()->unpolish(this);
    style()->polish(this);
    update();
}

TabBarButton::TabBarButton(QIcon const& icon, QWidget* parent)
    : QPushButton(icon, {}, parent)
{
    setObjectName("LadybirdTabButton");
    setFixedSize({ TAB_BUTTON_SIZE, TAB_BUTTON_SIZE });
    update_icon_size();
    setFocusPolicy(Qt::NoFocus);
    setFlat(true);
    hide();
}

void TabBarButton::set_icon_inset(int inset)
{
    m_icon_inset = inset;
    update_icon_size();
}

void TabBarButton::update_icon_size()
{
    auto collapsed = property(COLLAPSED_VERTICAL_TAB_BUTTON_PROPERTY).toBool();
    auto icon_size = (collapsed ? COLLAPSED_VERTICAL_TAB_ICON_SIZE : TAB_BUTTON_ICON_SIZE) - (2 * m_icon_inset);
    setIconSize({ icon_size, icon_size });
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

    QStylePainter painter(this);

    if (objectName() == "LadybirdTabButton") {
        auto collapsed = property(COLLAPSED_VERTICAL_TAB_BUTTON_PROPERTY).toBool();
        auto frame_style = collapsed
            ? ChromeStyle::CircularControlFrameStyle::ActiveTabOverlay
            : ChromeStyle::CircularControlFrameStyle::InteractionOnly;
        ChromeStyle::paint_circular_control_frame(painter, *this, frame_style);
    } else if (objectName() == "LadybirdAudioState") {
        ChromeStyle::paint_circular_control_frame(painter, *this);
    }

    painter.drawControl(QStyle::CE_PushButton, option);
}

}
