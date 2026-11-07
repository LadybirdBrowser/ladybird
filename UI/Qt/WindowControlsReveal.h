/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/kmalloc.h>

#include <QPointF>
#include <QRect>
#include <QWidget>

class QTimer;
class QVariantAnimation;

namespace Ladybird {

// With everything in the sidebar, there is no toolbar or tab strip to hold the window controls, and the page runs to
// the top of the window. So they stay out of sight until the pointer reaches the top of the window over the page's far
// corner, and then slide down over the page until it leaves them.
class WindowControlsReveal final : public QWidget {
public:
    AK_ALLOC_WITH_KMALLOC;

    WindowControlsReveal(QWidget& parent, QWidget& page);

    void hold(QWidget& window_controls);
    void release();

private:
    virtual bool eventFilter(QObject* watched, QEvent* event) override;
    virtual void paintEvent(QPaintEvent*) override;

    bool event_is_for_this_window(QObject* watched) const;
    QRect rect_when_revealed() const;
    void pointer_moved(QPointF global_position, bool button_is_down);
    void set_revealed(bool);
    void place();

    QWidget& m_page;
    QWidget* m_window_controls { nullptr };
    QTimer* m_conceal_timer { nullptr };
    QVariantAnimation* m_animation { nullptr };
    bool m_revealed { false };
    qreal m_progress { 0 };
};

}
