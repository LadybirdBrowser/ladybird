/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Time.h>
#include <LibCompositing/Export.h>
#include <LibGfx/Point.h>
#include <LibGfx/Size.h>

namespace Compositing {

// How far a touchpad has pulled one scroller past the edges of its scroll range, and how the scroller springs back.
// A stretch has the sign of the scroll offset direction it extends: a negative y is a scroller pulled past its top.
// Deltas, stretches and the scrollport are in the same pixels. Velocities are in those pixels per second.
class COMPOSITING_API ElasticOverscroll {
public:
    // A scroller that is shown magnified takes up `screen_pixels_per_pixel` pixels of the screen with each of its own.
    explicit ElasticOverscroll(Gfx::FloatSize scrollport_size, float screen_pixels_per_pixel = 1);

    // Whole pixels of the screen, like the scroll offsets the stretch is presented with.
    Gfx::FloatPoint stretch() const;
    bool is_at_rest() const { return !m_rebound.has_value() && m_held_stretch.is_zero(); }
    bool is_rebounding() const { return m_rebound.has_value(); }

    // The fingers moved by `delta`. The part of it that points back to the scroll range relaxes the stretch, and the
    // rest is returned to scroll with. The fingers take a rebound over from where it has got to.
    Gfx::FloatPoint relax_by(Gfx::FloatPoint delta);

    // The fingers moved by `overscroll_delta` further past an edge the scroller is already at.
    void pull_by(Gfx::FloatPoint overscroll_delta);

    // The fingers lifted, or momentum carried the scroller into its edge.
    void release(Gfx::FloatPoint velocity, MonotonicTime now);

    // Returns false once the scroller has sprung back.
    bool advance_rebound_to(MonotonicTime frame_time);

private:
    struct Rebound {
        MonotonicTime started_at;
        Gfx::FloatPoint initial_stretch;
        Gfx::FloatPoint initial_velocity;
        Gfx::FloatPoint current_stretch;
    };

    Gfx::FloatSize m_scrollport_size;
    float m_screen_pixels_per_pixel { 1 };
    Gfx::FloatPoint m_held_stretch;
    Optional<Rebound> m_rebound;
};

}
