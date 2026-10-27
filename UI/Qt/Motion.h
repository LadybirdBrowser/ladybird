/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <QEasingCurve>
#include <QPointF>

namespace Ladybird::Motion {

// The durations of the chrome's motion, in milliseconds.
// Hover out, and any state that must feel immediate.
constexpr inline int INSTANT = 0;
// Hover in, focus, menus, the active tab.
constexpr inline int FAST = 100;
// Tabs opening, closing and reordering.
constexpr inline int BASE = 160;
// The sidebar, and anything else that moves far.
constexpr inline int SLOW = 220;
// The way a menu or popover disappears.
constexpr inline int EXIT = 80;

inline QEasingCurve cubic_bezier(qreal x1, qreal y1, qreal x2, qreal y2)
{
    QEasingCurve curve(QEasingCurve::BezierSpline);
    curve.addCubicBezierSegment({ x1, y1 }, { x2, y2 }, { 1, 1 });
    return curve;
}

// For things that appear: fast at first, settling gently.
inline QEasingCurve enter_easing() { return cubic_bezier(0.2, 0, 0, 1); }
// For things that leave: slow at first, then gone.
inline QEasingCurve exit_easing() { return cubic_bezier(0.4, 0, 1, 1); }
// For things that move from one place to another.
inline QEasingCurve move_easing() { return cubic_bezier(0.3, 0, 0.2, 1); }

// How far along a motion that goes from rest to a peak and back is, from 0 at rest to 1 at the peak. Each half is eased
// as things that appear are.
inline qreal there_and_back(qreal progress, qreal peak_at)
{
    static auto const easing = enter_easing();
    if (progress < peak_at)
        return easing.valueForProgress(progress / peak_at);
    return 1 - easing.valueForProgress((progress - peak_at) / (1 - peak_at));
}

}
