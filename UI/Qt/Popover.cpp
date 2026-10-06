/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/Popover.h>

#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScreen>
#include <QVBoxLayout>

namespace Ladybird {

static constexpr int POPOVER_ANCHOR_GAP = 4;

// A message that asks whether to act on it.
MessagePopover::MessagePopover(QWidget* parent, int width, QString const& title, QString const& body, QString const& dismiss_text, QString const& accept_text)
    : QFrame(parent, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint)
{
    setObjectName("LadybirdMessagePopover");
#if defined(AK_OS_MACOS)
    setAttribute(Qt::WA_NativeWindow);
#endif
    setFrameShape(QFrame::StyledPanel);
    setFrameShadow(QFrame::Raised);
    setAutoFillBackground(true);
    setFixedWidth(width);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(10);

    auto* title_label = new QLabel(title, this);
    title_label->setObjectName("LadybirdMessagePopoverTitle");
    title_label->setWordWrap(true);
    layout->addWidget(title_label);

    auto* body_label = new QLabel(body, this);
    body_label->setObjectName("LadybirdMessagePopoverBody");
    body_label->setWordWrap(true);
    layout->addWidget(body_label);

    auto* button_layout = new QHBoxLayout;
    button_layout->setSpacing(8);
    button_layout->addStretch();
    layout->addLayout(button_layout);

    auto* dismiss_button = new QPushButton(dismiss_text, this);
    dismiss_button->setObjectName("LadybirdMessagePopoverDismissButton");
    dismiss_button->setFocusPolicy(Qt::NoFocus);
    QObject::connect(dismiss_button, &QPushButton::clicked, this, [this] { close(); });
    button_layout->addWidget(dismiss_button);

    auto* accept_button = new QPushButton(accept_text, this);
    accept_button->setObjectName("LadybirdMessagePopoverAcceptButton");
    accept_button->setDefault(true);
    QObject::connect(accept_button, &QPushButton::clicked, this, [this] {
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

// Places the popover below the anchor with their right edges aligned, kept on the anchor's screen, and above the anchor
// when there is no room below it.
void move_popover_below(QWidget& popover, QWidget& anchor)
{
    auto anchor_position = anchor.mapToGlobal(anchor.rect().bottomRight());
    auto popup_position = QPoint(anchor_position.x() - popover.width(), anchor_position.y() + POPOVER_ANCHOR_GAP);

    if (auto* screen = QGuiApplication::screenAt(anchor_position)) {
        auto available_geometry = screen->availableGeometry();
        if (popup_position.x() < available_geometry.left())
            popup_position.setX(available_geometry.left());
        if (popup_position.x() + popover.width() > available_geometry.right())
            popup_position.setX(available_geometry.right() - popover.width() + 1);
        if (popup_position.y() + popover.height() > available_geometry.bottom())
            popup_position.setY(anchor.mapToGlobal(anchor.rect().topRight()).y() - popover.height() - POPOVER_ANCHOR_GAP);
    }

    popover.move(popup_position);
}

}
