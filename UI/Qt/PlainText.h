/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/kmalloc.h>

#include <QObject>
#include <QPoint>
#include <QRect>
#include <QString>

class QWidget;

namespace Ladybird {

// Tooltips and labels can hold text from web pages, and Qt treats any text that looks like rich text as rich text.
//
// So all tooltips are shown by this filter instead, as plain text in a window of ours: Widgets keep their tooltip text
// as it is, and no widget gets to see a QEvent::ToolTip. A tooltip that isn't found here is not shown.
//
// Labels become Qt::PlainText as they are polished, unless they explicitly ask for another format. A label that has its
// heightForWidth() called before it is polished has to set its format itself.
class PlainTextEventFilter final : public QObject {
    Q_OBJECT

public:
    AK_ALLOC_WITH_KMALLOC;

    virtual bool eventFilter(QObject*, QEvent*) override;
};

struct ToolTipContent {
    QString text;
    QRect rect;
};

// If the rect isn't empty, the tooltip hides when the mouse leaves it. It is in the coordinates of the given widget.
void show_tooltip(QPoint global_position, QString const& text, QWidget& widget, QRect rect = {}, int display_time_ms = -1);
void hide_tooltip();

}
