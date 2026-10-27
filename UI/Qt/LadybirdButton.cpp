/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericShorthands.h>
#include <LibWebView/Application.h>
#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/Icon.h>
#include <UI/Qt/LadybirdButton.h>
#include <UI/Qt/Motion.h>
#include <UI/Qt/SystemAppearance.h>
#include <UI/Qt/Theme.h>

#include <QFocusEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QVariantAnimation>

namespace Ladybird {

static constexpr int TILE_HEIGHT = 26;
static constexpr qreal TILE_RADIUS = 7;
// NB: The button is as tall as the tab row it sits in on the strip.
static constexpr int TILE_VERTICAL_MARGIN = 4;
// The tile holds the mark and a menu chevron, padded a little more before the mark than after the chevron.
static constexpr qreal TILE_LEADING_PADDING = 7;
static constexpr qreal TILE_TRAILING_PADDING = 6;
static constexpr qreal MARK_CHEVRON_GAP = 3;
static constexpr QSizeF LOGOMARK_SIZE { 20, 16.7 };
static constexpr qreal LOGOMARK_STROKE_WIDTH = 1.55;
static constexpr qreal PRIVATE_GLYPH_SIZE = 20;
static constexpr qreal CHEVRON_SIZE = 10;
// How deep the tile's fill is under the pointer, and while pressed or while the menu is open. At rest it has none, only
// its hairline, so it doesn't read as stuck in a hover.
static constexpr qreal HOVERED_FILL = 0.75;
static constexpr qreal PRESSED_FILL = 1;

static qreal mark_width(WebView::IsPrivate is_private)
{
    return is_private == WebView::IsPrivate::Yes ? PRIVATE_GLYPH_SIZE : LOGOMARK_SIZE.width();
}

static QSize tile_size(WebView::IsPrivate is_private)
{
    auto width = TILE_LEADING_PADDING + mark_width(is_private) + MARK_CHEVRON_GAP + CHEVRON_SIZE + TILE_TRAILING_PADDING;
    return { static_cast<int>(ceil(width)), TILE_HEIGHT };
}

// The menu chevron points down, drawn in a 16 px box scaled to its size.
static void paint_chevron(QPainter& painter, QPointF center, QColor const& color)
{
    QPainterPath chevron;
    chevron.moveTo(4, 6.5);
    chevron.lineTo(8, 10.5);
    chevron.lineTo(12, 6.5);

    painter.save();
    painter.translate(center - QPointF(CHEVRON_SIZE / 2, CHEVRON_SIZE / 2));
    painter.scale(CHEVRON_SIZE / 16, CHEVRON_SIZE / 16);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(color, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.drawPath(chevron);
    painter.restore();
}

// The private glyph is a domino mask with its eye holes cut out, drawn in a 16 px box scaled to its size.
static void paint_private_glyph(QPainter& painter, QPointF center, QColor const& color)
{
    QPainterPath glyph;
    glyph.setFillRule(Qt::OddEvenFill);
    glyph.moveTo(8, 5.3);
    glyph.cubicTo(9.7, 4.5, 12, 4.1, 13.9, 4.4);
    glyph.cubicTo(14.8, 4.6, 15.1, 5.4, 15, 6.6);
    glyph.cubicTo(14.8, 9.6, 13.5, 11.8, 11.2, 11.8);
    glyph.cubicTo(9.8, 11.8, 8.9, 10.9, 8, 9.9);
    glyph.cubicTo(7.1, 10.9, 6.2, 11.8, 4.8, 11.8);
    glyph.cubicTo(2.5, 11.8, 1.2, 9.6, 1, 6.6);
    glyph.cubicTo(0.9, 5.4, 1.2, 4.6, 2.1, 4.4);
    glyph.cubicTo(4, 4.1, 6.3, 4.5, 8, 5.3);
    glyph.closeSubpath();
    glyph.moveTo(3.1, 7.3);
    glyph.cubicTo(3.5, 8.7, 5.7, 9.1, 6.7, 7.9);
    glyph.cubicTo(6.5, 6.6, 4.1, 6.2, 3.1, 7.3);
    glyph.closeSubpath();
    glyph.moveTo(12.9, 7.3);
    glyph.cubicTo(12.5, 8.7, 10.3, 9.1, 9.3, 7.9);
    glyph.cubicTo(9.5, 6.6, 11.9, 6.2, 12.9, 7.3);
    glyph.closeSubpath();

    painter.save();
    painter.translate(center - QPointF(PRIVATE_GLYPH_SIZE / 2, PRIVATE_GLYPH_SIZE / 2));
    painter.scale(PRIVATE_GLYPH_SIZE / 16, PRIVATE_GLYPH_SIZE / 16);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawPath(glyph);
    painter.restore();
}

LadybirdButton::LadybirdButton(WebView::IsPrivate is_private, QWidget* parent)
    : QToolButton(parent)
    , m_is_private(is_private)
{
    setObjectName("LadybirdButton");
    setAccessibleName(is_private == WebView::IsPrivate::Yes ? "Ladybird menu, private window" : "Ladybird menu");
    setToolTip(is_private == WebView::IsPrivate::Yes ? "Ladybird menu, private window" : "Ladybird menu");
    setFocusPolicy(Qt::TabFocus);
    setPopupMode(QToolButton::InstantPopup);
    setFixedSize(sizeHint());

    // The tile fills in under the pointer, and a little more while pressed or while the menu is open.
    m_fill_animation = new QVariantAnimation(this);
    m_fill_animation->setDuration(Motion::FAST);
    connect(m_fill_animation, &QVariantAnimation::valueChanged, this, [this](QVariant const& value) {
        m_fill = value.toReal();
        update();
    });
}

void LadybirdButton::animate_fill(qreal fill)
{
    m_fill_animation->stop();
    if (SystemAppearance::prefers_reduced_motion()) {
        m_fill = fill;
        update();
        return;
    }
    m_fill_animation->setStartValue(m_fill);
    m_fill_animation->setEndValue(fill);
    m_fill_animation->start();
}

QSize LadybirdButton::sizeHint() const
{
    auto tile = tile_size(m_is_private);
    return { tile.width(), tile.height() + (2 * TILE_VERTICAL_MARGIN) };
}

QRect LadybirdButton::tile_rect() const
{
    auto tile = tile_size(m_is_private);
    return { QPoint((width() - tile.width()) / 2, ((height() - tile.height()) / 2) + m_tile_vertical_offset), tile };
}

// NB: On the tab strip, the button sits on the same line as the tabs, a little below the middle of its own height.
void LadybirdButton::set_tile_vertical_offset(int offset)
{
    if (m_tile_vertical_offset == offset)
        return;
    m_tile_vertical_offset = offset;
    update();
}

void LadybirdButton::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    // The button fades back in an inactive window.
    painter.setOpacity(ChromeStyle::window_activity(*this) == ChromeStyle::WindowActivity::Inactive ? 0.55 : 1.0);

    // The tile is a hairline outline, with a neutral fill only under the pointer or while pressed. The mark is in the
    // profile's color, which is the only brand color in the chrome, and doesn't change with the tile's state. A private
    // window shows its glyph in place of the logomark.
    auto is_private = m_is_private == WebView::IsPrivate::Yes;
    auto dark = is_private || Theme::is_dark(*this);
    auto fill_color = dark ? QColor(255, 255, 255) : QColor(0, 0, 0);
    fill_color.setAlphaF((dark ? 0.16 : 0.12) * m_fill);
    auto hairline_color = fill_color;
    hairline_color.setAlphaF(Theme::has_increased_contrast() ? 0.45 : (dark ? 0.16 : 0.14));
    auto mark_color = is_private ? QColor(0xd8c8ff) : (dark ? QColor(0xa684ff) : QColor(0x4b22ad));

    QRectF tile_rect { this->tile_rect() };
    painter.setPen(Qt::NoPen);
    painter.setBrush(fill_color);
    painter.drawRoundedRect(tile_rect, TILE_RADIUS, TILE_RADIUS);
    painter.setPen(QPen(hairline_color, 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(tile_rect.adjusted(0.5, 0.5, -0.5, -0.5), TILE_RADIUS - 0.5, TILE_RADIUS - 0.5);

    auto mark_center = QPointF(tile_rect.left() + TILE_LEADING_PADDING + (mark_width(m_is_private) / 2), tile_rect.center().y());
    if (is_private) {
        paint_private_glyph(painter, mark_center, mark_color);
    } else {
        QRectF logomark_rect { QPointF(), LOGOMARK_SIZE };
        logomark_rect.moveCenter(mark_center);
        paint_logomark(painter, logomark_rect, mark_color, LOGOMARK_STROKE_WIDTH);
    }
    auto chevron_center = QPointF(tile_rect.right() - TILE_TRAILING_PADDING - (CHEVRON_SIZE / 2), tile_rect.center().y());
    paint_chevron(painter, chevron_center, mark_color);

    // Focus is ringed only when it came from the keyboard.
    if (hasFocus() && m_has_keyboard_focus) {
        painter.setOpacity(1);
        ChromeStyle::paint_focus_ring(painter, tile_rect, TILE_RADIUS);
    }
}

// The fill deepens as the pointer comes over the button, and is gone at once as it leaves.
void LadybirdButton::enterEvent(QEnterEvent* event)
{
    QToolButton::enterEvent(event);
    if (!isDown())
        animate_fill(HOVERED_FILL);
}

void LadybirdButton::leaveEvent(QEvent* event)
{
    QToolButton::leaveEvent(event);
    if (isDown())
        return;
    m_fill_animation->stop();
    m_fill = 0;
    update();
}

void LadybirdButton::focusInEvent(QFocusEvent* event)
{
    m_has_keyboard_focus = first_is_one_of(event->reason(), Qt::TabFocusReason, Qt::BacktabFocusReason, Qt::ShortcutFocusReason);
    QToolButton::focusInEvent(event);
}

// Menus open as the button is pressed, so pressing, dragging to an item and releasing chooses it.
void LadybirdButton::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        show_menu();
        return;
    }
    QToolButton::mousePressEvent(event);
}

void LadybirdButton::keyPressEvent(QKeyEvent* event)
{
    if (first_is_one_of(event->key(), Qt::Key_Select, Qt::Key_Space, Qt::Key_Return, Qt::Key_Enter, Qt::Key_Down)) {
        show_menu();
        return;
    }
    QToolButton::keyPressEvent(event);
}

void LadybirdButton::show_menu()
{
    auto* menu = this->menu();
    if (!menu)
        return;

    setDown(true);
    animate_fill(PRESSED_FILL);
    // The menu hangs from the tile's bottom left corner, a few points below it.
    auto tile = tile_size(m_is_private);
    auto tile_bottom_left = QPoint((width() - tile.width()) / 2, (height() + tile.height()) / 2 + 4);
    menu->exec(mapToGlobal(tile_bottom_left));
    setDown(false);
    animate_fill(underMouse() ? HOVERED_FILL : 0);
}

}
