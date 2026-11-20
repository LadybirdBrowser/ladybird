/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/ElidedLabel.h>
#include <UI/Qt/Icon.h>
#include <UI/Qt/NotificationBar.h>
#include <UI/Qt/Theme.h>

#include <QEvent>
#include <QHBoxLayout>
#include <QPainter>
#include <QPushButton>
#include <QToolButton>

namespace Ladybird {

static constexpr int BAR_HEIGHT = 32;
static constexpr int DIVIDER_HEIGHT = 1;
static constexpr int ICON_SIZE = 16;
static constexpr int CLOSE_ICON_SIZE = 12;
static constexpr int CLOSE_BUTTON_SIZE = 24;
static constexpr int HORIZONTAL_INSET = 12;
static constexpr int ICON_TEXT_SPACING = 8;

NotificationBar::NotificationBar(QWidget& parent, QString const& message, Vector<Button> buttons, Function<void()> on_close)
    : QWidget(&parent)
{
    setObjectName("LadybirdNotificationBar");
    setAttribute(Qt::WA_OpaquePaintEvent);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    setFixedHeight(BAR_HEIGHT + DIVIDER_HEIGHT);
    setAccessibleName(message);

    auto* layout = new QHBoxLayout(this);
    auto end_inset = HORIZONTAL_INSET;
    // NB: The close icon ends at the inset, like the warning icon starts at it.
    if (on_close)
        end_inset -= (CLOSE_BUTTON_SIZE - CLOSE_ICON_SIZE) / 2;
    layout->setContentsMargins(HORIZONTAL_INSET, 0, end_inset, DIVIDER_HEIGHT);
    layout->setSpacing(ICON_TEXT_SPACING);

    m_icon_area = new QWidget(this);
    m_icon_area->setFixedSize(ICON_SIZE, ICON_SIZE);
    layout->addWidget(m_icon_area);

    layout->addWidget(new ElidedLabel(message, Qt::ElideRight, this), 1);

    for (auto& button : buttons) {
        auto* push_button = new QPushButton(button.label, this);
        push_button->setCursor(Qt::PointingHandCursor);
        QObject::connect(push_button, &QPushButton::clicked, this, [on_click = AK::move(button.on_click)] {
            on_click();
        });
        layout->addWidget(push_button);
    }

    if (on_close) {
        m_close_button = new QToolButton(this);
        m_close_button->setAutoRaise(true);
        m_close_button->setFixedSize(CLOSE_BUTTON_SIZE, CLOSE_BUTTON_SIZE);
        m_close_button->setIconSize({ CLOSE_ICON_SIZE, CLOSE_ICON_SIZE });
        m_close_button->setToolTip("Dismiss");
        m_close_button->setAccessibleName("Dismiss");
        QObject::connect(m_close_button, &QToolButton::clicked, this, [on_close = AK::move(on_close)] {
            on_close();
        });
        layout->addWidget(m_close_button);
    }

    update_chrome_style();
}

bool NotificationBar::event(QEvent* event)
{
    if (event->type() == QEvent::PaletteChange)
        update_chrome_style();
    return QWidget::event(event);
}

void NotificationBar::update_chrome_style()
{
    // NB: Setting a style sheet changes the palette, so it is only set when it changed, or it would set itself forever.
    if (auto style_sheet = ChromeStyle::notification_bar_style_sheet(Theme::chrome_palette(*this)); styleSheet() != style_sheet)
        setStyleSheet(style_sheet);
    if (m_close_button)
        m_close_button->setIcon(create_chrome_icon(ChromeIcon::Close, Theme::chrome(*this).icon));
}

void NotificationBar::paintEvent(QPaintEvent*)
{
    auto const& chrome = Theme::chrome(*this);

    QPainter painter(this);
    painter.fillRect(QRect { 0, 0, width(), BAR_HEIGHT }, chrome.toolbar);
    painter.fillRect(QRect { 0, BAR_HEIGHT, width(), DIVIDER_HEIGHT }, chrome.divider);
    create_chrome_icon(ChromeIcon::Warning, chrome.warning).paint(&painter, m_icon_area->geometry());
}

}
