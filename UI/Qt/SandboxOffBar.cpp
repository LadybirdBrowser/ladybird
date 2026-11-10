/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/Icon.h>
#include <UI/Qt/SandboxOffBar.h>
#include <UI/Qt/Theme.h>

#include <QEvent>
#include <QPainter>

namespace Ladybird {

static constexpr int BAR_HEIGHT = 32;
static constexpr int DIVIDER_HEIGHT = 1;
static constexpr int ICON_SIZE = 16;
static constexpr int HORIZONTAL_INSET = 12;
static constexpr int ICON_TEXT_SPACING = 8;

static constexpr auto MESSAGE = "The sandbox is off. Websites are less isolated from your computer.";

SandboxOffBar::SandboxOffBar(QWidget* parent)
    : QWidget(parent)
{
    setObjectName("LadybirdSandboxOffBar");
    setAttribute(Qt::WA_OpaquePaintEvent);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    setFixedHeight(BAR_HEIGHT + DIVIDER_HEIGHT);
    setAccessibleName(MESSAGE);
}

bool SandboxOffBar::event(QEvent* event)
{
    switch (event->type()) {
    case QEvent::PaletteChange:
    case QEvent::FontChange:
    case QEvent::LayoutDirectionChange:
        update();
        break;
    default:
        break;
    }
    return QWidget::event(event);
}

void SandboxOffBar::paintEvent(QPaintEvent*)
{
    auto const& chrome = Theme::chrome(*this);

    QPainter painter(this);
    painter.fillRect(QRect { 0, 0, width(), BAR_HEIGHT }, chrome.toolbar);
    painter.fillRect(QRect { 0, BAR_HEIGHT, width(), DIVIDER_HEIGHT }, chrome.divider);

    QRect content_rect { HORIZONTAL_INSET, 0, width() - 2 * HORIZONTAL_INSET, BAR_HEIGHT };
    QRect icon_rect { 0, (BAR_HEIGHT - ICON_SIZE) / 2, ICON_SIZE, ICON_SIZE };
    QRect text_rect { 0, 0, content_rect.width() - ICON_SIZE - ICON_TEXT_SPACING, BAR_HEIGHT };
    if (layoutDirection() == Qt::RightToLeft) {
        icon_rect.moveRight(content_rect.right());
        text_rect.moveRight(icon_rect.left() - ICON_TEXT_SPACING - 1);
    } else {
        icon_rect.moveLeft(content_rect.left());
        text_rect.moveLeft(icon_rect.right() + 1 + ICON_TEXT_SPACING);
    }

    create_chrome_icon(ChromeIcon::Warning, chrome.warning).paint(&painter, icon_rect);

    painter.setPen(chrome.text);
    auto text = fontMetrics().elidedText(MESSAGE, Qt::ElideRight, text_rect.width());
    painter.drawText(text_rect, Qt::AlignVCenter | Qt::AlignLeading, text);
}

}
