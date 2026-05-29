/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/DevToolsBanner.h>
#include <UI/Qt/StringUtils.h>

#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>

namespace Ladybird {

DevToolsBanner::DevToolsBanner(QWidget* parent)
    : QWidget(parent)
{
    setObjectName("LadybirdDevToolsBanner");
    setAttribute(Qt::WA_StyledBackground);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    update_chrome_style();

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(12, 3, 12, 3);
    layout->setSpacing(6);

    m_label = new QLabel(this);
    layout->addWidget(m_label);
    layout->addStretch();

    auto* disable_button = new QPushButton("Disable", this);
    connect(disable_button, &QPushButton::clicked, this, &DevToolsBanner::disable_requested);
    layout->addWidget(disable_button);
}

void DevToolsBanner::set_port(u16 port)
{
    m_label->setText(qformatted("DevTools is enabled on port {}", port));
}

bool DevToolsBanner::event(QEvent* event)
{
    if (event->type() == QEvent::PaletteChange)
        update_chrome_style();

    return QWidget::event(event);
}

void DevToolsBanner::update_chrome_style()
{
    if (m_is_updating_chrome_style)
        return;

    m_is_updating_chrome_style = true;
    setStyleSheet(ChromeStyle::devtools_banner_style_sheet(palette()));
    m_is_updating_chrome_style = false;
}

}
