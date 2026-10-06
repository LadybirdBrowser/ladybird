/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StdLibExtras.h>
#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/Popover.h>

#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QLabel>
#include <QPushButton>
#include <QScreen>
#include <QVBoxLayout>

namespace Ladybird {

static constexpr int POPOVER_ANCHOR_GAP = 4;
static constexpr int POPOVER_SHADOW_MARGIN = 24;

// The window is transparent around a card that casts its own shadow. The window has no shadow of its own, which would
// not follow the card's rounded corners.
Popover::Popover(QWidget* parent)
    : QWidget(parent, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint)
{
#if defined(AK_OS_MACOS)
    setAttribute(Qt::WA_NativeWindow);
#endif
    setAttribute(Qt::WA_TranslucentBackground);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(POPOVER_SHADOW_MARGIN, POPOVER_SHADOW_MARGIN, POPOVER_SHADOW_MARGIN, POPOVER_SHADOW_MARGIN);
    layout->setSpacing(0);

    m_card = new QFrame(this);
    m_card->setFrameShape(QFrame::StyledPanel);
    m_card->setFrameShadow(QFrame::Raised);
    layout->addWidget(m_card);

    auto* shadow = new QGraphicsDropShadowEffect(m_card);
    shadow->setBlurRadius(24);
    shadow->setOffset(0, 4);
    shadow->setColor(QColor(0, 0, 0, 72));
    m_card->setGraphicsEffect(shadow);
}

// A message that asks whether to act on it. Leaving the popover in any other way than accepting it, such as by clicking
// outside it, dismisses the message.
MessagePopover::MessagePopover(QWidget* parent, int width, QString const& title, QString const& body, QString const& dismiss_text, QString const& accept_text)
    : Popover(parent)
{
    card().setObjectName("LadybirdMessagePopover");
    card().setFixedWidth(width);

    auto* layout = new QVBoxLayout(&card());
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(10);

    auto* title_label = new QLabel(title, &card());
    title_label->setObjectName("LadybirdMessagePopoverTitle");
    title_label->setWordWrap(true);
    layout->addWidget(title_label);

    auto* body_label = new QLabel(body, &card());
    body_label->setObjectName("LadybirdMessagePopoverBody");
    body_label->setWordWrap(true);
    layout->addWidget(body_label);

    auto* button_layout = new QHBoxLayout;
    button_layout->setSpacing(8);
    button_layout->addStretch();
    layout->addLayout(button_layout);

    auto* dismiss_button = new QPushButton(dismiss_text, &card());
    dismiss_button->setObjectName("LadybirdMessagePopoverDismissButton");
    dismiss_button->setFocusPolicy(Qt::NoFocus);
    QObject::connect(dismiss_button, &QPushButton::clicked, this, [this] { close(); });
    button_layout->addWidget(dismiss_button);

    auto* accept_button = new QPushButton(accept_text, &card());
    accept_button->setObjectName("LadybirdMessagePopoverAcceptButton");
    accept_button->setDefault(true);
    QObject::connect(accept_button, &QPushButton::clicked, this, [this] {
        m_is_accepted = true;
        close();
        if (on_accept)
            on_accept();
    });
    button_layout->addWidget(accept_button);
}

void MessagePopover::update_chrome_style(QPalette const& palette)
{
    setPalette(palette);
    setStyleSheet(ChromeStyle::message_popover_style_sheet(palette));
}

void MessagePopover::hideEvent(QHideEvent* event)
{
    Popover::hideEvent(event);
    if (!exchange(m_is_accepted, false) && on_dismiss)
        on_dismiss();
}

// Places the popover's card below the anchor with their right edges aligned, kept on the anchor's screen, and above the
// anchor when there is no room below it. Only the shadow around the card may reach past the edge of the screen.
void move_popover_below(Popover& popover, QWidget& anchor)
{
    auto card_size = popover.size().shrunkBy({ POPOVER_SHADOW_MARGIN, POPOVER_SHADOW_MARGIN, POPOVER_SHADOW_MARGIN, POPOVER_SHADOW_MARGIN });
    auto anchor_position = anchor.mapToGlobal(anchor.rect().bottomRight());
    auto card_position = QPoint(anchor_position.x() - card_size.width(), anchor_position.y() + POPOVER_ANCHOR_GAP);

    if (auto* screen = QGuiApplication::screenAt(anchor_position)) {
        auto available_geometry = screen->availableGeometry();
        if (card_position.x() < available_geometry.left())
            card_position.setX(available_geometry.left());
        if (card_position.x() + card_size.width() > available_geometry.right())
            card_position.setX(available_geometry.right() - card_size.width() + 1);
        if (card_position.y() + card_size.height() > available_geometry.bottom())
            card_position.setY(anchor.mapToGlobal(anchor.rect().topRight()).y() - card_size.height() - POPOVER_ANCHOR_GAP);
        if (card_position.y() < available_geometry.top())
            card_position.setY(available_geometry.top());
    }

    popover.move(card_position - QPoint(POPOVER_SHADOW_MARGIN, POPOVER_SHADOW_MARGIN));
}

}
