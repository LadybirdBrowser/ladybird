/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/CircularQueue.h>
#include <AK/Optional.h>
#include <AK/Time.h>
#include <LibCompositing/Export.h>
#include <LibGfx/Point.h>

namespace Compositing {

// Velocities are in wheel delta units per second. Displacements are in wheel delta units.

// Calculates the velocity of a touchpad scroll gesture when the fingers lift.
class COMPOSITING_API ScrollVelocityTracker {
public:
    void add_step(Gfx::FloatPoint delta, MonotonicTime);
    void reset() { m_steps.clear(); }

    // Returns no value if the fingers stopped for too long before the lift.
    Optional<Gfx::FloatPoint> velocity_at_lift(MonotonicTime lift_time) const;

private:
    struct Step {
        Gfx::FloatPoint delta;
        MonotonicTime time;
    };

    CircularQueue<Step, 20> m_steps;
};

// Continues to scroll after a touchpad flick, for platforms that do not send momentum scroll events.
// The speed decreases by the same fraction per frame as a momentum smooth scroll.
// MomentumFlingEstimator uses this decrease to find where the scroll stops.
class COMPOSITING_API ScrollFling {
public:
    static Optional<ScrollFling> start(Gfx::FloatPoint velocity, MonotonicTime now);

    // Returns the displacement since the previous step, or no value after the fling stops.
    Optional<Gfx::FloatPoint> step(MonotonicTime now);
    Gfx::FloatPoint velocity_at(MonotonicTime) const;

private:
    ScrollFling(Gfx::FloatPoint velocity, MonotonicTime now);

    double decay_at(double elapsed_seconds) const;

    Gfx::FloatPoint m_initial_velocity;
    double m_initial_speed { 0 };
    MonotonicTime m_started_at;
    double m_last_step_elapsed_seconds { 0 };
    double m_last_step_decay { 1 };
    bool m_finished { false };
};

// If the user flicks again while a fling is fast, the new fling gets the velocity of the old fling added.
class COMPOSITING_API ScrollFlingBooster {
public:
    void fling_was_interrupted(Gfx::FloatPoint velocity, MonotonicTime now);
    Gfx::FloatPoint boosted_velocity(Gfx::FloatPoint velocity, MonotonicTime now);
    void reset() { m_interrupted_fling.clear(); }

private:
    struct InterruptedFling {
        Gfx::FloatPoint velocity;
        MonotonicTime interrupted_at;
    };
    Optional<InterruptedFling> m_interrupted_fling;
};

}
