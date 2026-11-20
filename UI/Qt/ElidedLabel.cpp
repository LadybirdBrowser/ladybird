/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/ElidedLabel.h>

#include <QPainter>

namespace Ladybird {

ElidedLabel::ElidedLabel(QString text, Qt::TextElideMode elide_mode, QWidget* parent)
    : QLabel(text, parent)
    , m_elide_mode(elide_mode)
{
    setMinimumWidth(0);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
}

QSize ElidedLabel::sizeHint() const
{
    auto size = QLabel::sizeHint();
    size.setWidth(0);
    return size;
}

QSize ElidedLabel::minimumSizeHint() const
{
    return { 0, QLabel::minimumSizeHint().height() };
}

void ElidedLabel::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setFont(font());
    painter.setPen(palette().color(foregroundRole()));
    painter.drawText(rect(), alignment() | Qt::TextSingleLine, fontMetrics().elidedText(text(), m_elide_mode, width()));
}

}
