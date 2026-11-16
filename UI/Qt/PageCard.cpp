/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/PageCard.h>
#include <UI/Qt/Popover.h>
#include <UI/Qt/Theme.h>

#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#if defined(AK_OS_MACOS)
#    include <UI/Qt/MacWindow.h>
#endif

namespace Ladybird {

// A card over the top of the page tells the user something, with buttons that each close it. The card is a native
// child of the window, so it stacks above the native view of each tab's page once it is raised above it.
static constexpr int CARD_WIDTH = 580;
static constexpr int CARD_CORNER_RADIUS = 12;
static constexpr int PAGE_TOP_INSET = 22;

PageCard::PageCard(QWidget& parent, QString const& title, QString const& text, Vector<Button> buttons)
    : QFrame(&parent)
{
    setObjectName("LadybirdPageCard");
    setAccessibleName(title);
#if defined(AK_OS_MACOS)
    setAttribute(Qt::WA_DontCreateNativeAncestors);
    setAttribute(Qt::WA_NativeWindow);
#endif
    setFixedWidth(CARD_WIDTH);
    cast_floating_shadow(*this, CARD_CORNER_RADIUS);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(18, 14, 14, 14);
    layout->setSpacing(14);

    auto* texts = new QVBoxLayout;
    texts->setSpacing(3);
    auto* title_label = new QLabel(title, this);
    title_label->setObjectName("LadybirdPageCardTitle");
    texts->addWidget(title_label);
    auto* text_label = new QLabel(text, this);
    text_label->setObjectName("LadybirdPageCardText");
    text_label->setWordWrap(true);
    text_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    texts->addWidget(text_label);
    layout->addLayout(texts, 1);

    for (auto& button : buttons) {
        auto* push_button = new QPushButton(button.label, this);
        auto is_primary = button.role == ButtonRole::Primary;
        push_button->setObjectName(is_primary ? "LadybirdPageCardPrimaryButton" : "LadybirdPageCardSecondaryButton");
        push_button->setDefault(is_primary);
        QObject::connect(push_button, &QPushButton::clicked, this, [this, on_click = AK::move(button.on_click)] {
            if (on_click)
                on_click();
            deleteLater();
        });
        layout->addWidget(push_button);
    }

    update_chrome_style();
    adjustSize();
}

void PageCard::reposition(QWidget& page, int top_offset)
{
    if (m_page != &page)
        raise();
    m_page = &page;
    auto page_top_left = page.mapTo(parentWidget(), QPoint(0, 0));
    auto position = QPoint(page_top_left.x() + ((page.width() - width()) / 2), page_top_left.y() + PAGE_TOP_INSET + top_offset);
    if (pos() != position)
        move(position);
}

bool PageCard::event(QEvent* event)
{
    if (event->type() == QEvent::PaletteChange)
        update_chrome_style();
    return QFrame::event(event);
}

void PageCard::showEvent(QShowEvent* event)
{
    QFrame::showEvent(event);
#if defined(AK_OS_MACOS)
    round_appkit_view_corners(*this, CARD_CORNER_RADIUS, Theme::panel().edge);
#endif
}

void PageCard::update_chrome_style()
{
    // NB: Setting a style sheet changes the palette, so it is only set when it changed, or it would set itself forever.
    if (auto style_sheet = ChromeStyle::page_card_style_sheet(); styleSheet() != style_sheet)
        setStyleSheet(style_sheet);
}

}
