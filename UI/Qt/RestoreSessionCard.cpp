/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/Popover.h>
#include <UI/Qt/RestoreSessionCard.h>
#include <UI/Qt/StringUtils.h>
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

// The card is a native child of the tab, so it stacks above the native view the page is presented in.
static constexpr int CARD_WIDTH = 580;
static constexpr int CARD_CORNER_RADIUS = 12;
static constexpr int PAGE_TOP_INSET = 22;

RestoreSessionCard::RestoreSessionCard(QWidget& parent, QWidget& page, size_t tab_count)
    : QFrame(&parent)
    , m_page(page)
{
    setObjectName("LadybirdRestoreSessionCard");
    setAccessibleName("Ladybird quit unexpectedly");
#if defined(AK_OS_MACOS)
    setAttribute(Qt::WA_DontCreateNativeAncestors);
    setAttribute(Qt::WA_NativeWindow);
#endif
    setFixedWidth(CARD_WIDTH);
    cast_floating_shadow(*this, CARD_CORNER_RADIUS);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(18, 14, 14, 14);
    layout->setSpacing(14);

    auto* text = new QVBoxLayout;
    text->setSpacing(3);
    auto* title = new QLabel("Ladybird quit unexpectedly", this);
    title->setObjectName("LadybirdRestoreSessionTitle");
    text->addWidget(title);
    auto* question = new QLabel(tab_count == 1 ? QStringLiteral("Restore the tab you had open?") : qformatted("Restore the {} tabs you had open?", tab_count), this);
    question->setObjectName("LadybirdRestoreSessionQuestion");
    text->addWidget(question);
    layout->addLayout(text, 1);

    auto* not_now_button = new QPushButton("Not now", this);
    not_now_button->setObjectName("LadybirdRestoreSessionNotNow");
    QObject::connect(not_now_button, &QPushButton::clicked, this, [this] { deleteLater(); });
    layout->addWidget(not_now_button);

    auto* restore_button = new QPushButton("Restore tabs", this);
    restore_button->setObjectName("LadybirdRestoreSessionRestore");
    restore_button->setDefault(true);
    QObject::connect(restore_button, &QPushButton::clicked, this, [this] {
        if (on_restore)
            on_restore();
        deleteLater();
    });
    layout->addWidget(restore_button);

    update_chrome_style();
    adjustSize();
}

void RestoreSessionCard::reposition()
{
    auto page_top_left = m_page.mapTo(parentWidget(), QPoint(0, 0));
    auto position = QPoint(page_top_left.x() + ((m_page.width() - width()) / 2), page_top_left.y() + PAGE_TOP_INSET);
    if (pos() != position)
        move(position);
}

bool RestoreSessionCard::event(QEvent* event)
{
    if (event->type() == QEvent::PaletteChange)
        update_chrome_style();
    return QFrame::event(event);
}

void RestoreSessionCard::showEvent(QShowEvent* event)
{
    QFrame::showEvent(event);
    reposition();
    raise();
#if defined(AK_OS_MACOS)
    round_appkit_view_corners(*this, CARD_CORNER_RADIUS, Theme::panel().edge);
#endif
}

void RestoreSessionCard::update_chrome_style()
{
    // NB: Setting a style sheet changes the palette, so it is only set when it changed, or it would set itself forever.
    if (auto style_sheet = ChromeStyle::restore_session_card_style_sheet(); styleSheet() != style_sheet)
        setStyleSheet(style_sheet);
}

}
