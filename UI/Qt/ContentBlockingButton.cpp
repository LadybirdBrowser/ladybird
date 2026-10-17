/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/ContentBlockingButton.h>
#include <UI/Qt/Icon.h>
#include <UI/Qt/StringUtils.h>

#include <QEvent>
#include <QPainter>
#include <QStyleOptionToolButton>
#include <QStylePainter>

namespace Ladybird {

ContentBlockingButton::ContentBlockingButton(QWidget* parent)
    : QToolButton(parent)
{
    setObjectName("ContentBlockingButton");
    setAccessibleName("Ad blocking");
    setAutoRaise(true);
    setFixedSize(sizeHint());
    setFocusPolicy(Qt::StrongFocus);
    m_icon = create_chrome_icon(ChromeIcon::ContentBlocking, palette());
}

void ContentBlockingButton::changeEvent(QEvent* event)
{
    QToolButton::changeEvent(event);
    if (event->type() == QEvent::PaletteChange) {
        m_icon = create_chrome_icon(ChromeIcon::ContentBlocking, palette());
        update();
    }
}

void ContentBlockingButton::set_snapshot(WebView::ContentBlockingSnapshot const& snapshot)
{
    m_snapshot = snapshot;
    setEnabled(snapshot.state != WebView::ContentBlockingState::Unavailable);
    update();
}

void ContentBlockingButton::set_show_count_badge(bool show)
{
    if (m_show_count_badge == show)
        return;
    m_show_count_badge = show;
    update();
}

QString ContentBlockingButton::badge_text() const
{
    if (!m_show_count_badge || !m_snapshot.enabled() || m_snapshot.blocked_request_count == 0)
        return {};
    return m_snapshot.blocked_request_count > 99 ? "99+" : qformatted("{}", m_snapshot.blocked_request_count);
}

void ContentBlockingButton::paintEvent(QPaintEvent*)
{
    QStyleOptionToolButton option;
    initStyleOption(&option);
    option.icon = {};

    QStylePainter painter(this);
    ChromeStyle::paint_circular_control_frame(painter, *this);
    painter.drawComplexControl(QStyle::CC_ToolButton, option);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.save();

    if (!m_snapshot.enabled())
        painter.setOpacity(0.45);

    auto icon_rect = QRect((width() - 20) / 2, (height() - 20) / 2, 20, 20);
    m_icon.paint(&painter, icon_rect, Qt::AlignCenter, underMouse() || isDown() ? QIcon::Active : QIcon::Normal);
    painter.restore();

    auto badge = badge_text();
    if (badge.isEmpty())
        return;

    auto badge_font = font();
    badge_font.setPointSizeF(7.5);
    badge_font.setBold(true);

    painter.setFont(badge_font);

    auto badge_width = painter.fontMetrics().horizontalAdvance(badge) + 6;
    auto badge_height = painter.fontMetrics().height() + 2;
    auto badge_rect = QRect(width() - badge_width - 1, 1, badge_width, badge_height);

    painter.setPen(Qt::NoPen);
    painter.setBrush(ChromeStyle::chrome_accent());
    painter.drawRoundedRect(badge_rect, 5, 5);

    painter.setPen(Qt::white);
    painter.drawText(badge_rect, Qt::AlignCenter, badge);
}

}
