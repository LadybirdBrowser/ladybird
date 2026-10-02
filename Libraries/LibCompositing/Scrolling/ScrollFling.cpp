/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibCompositing/Scrolling/ScrollFling.h>
#include <LibCompositing/Scrolling/SmoothScrollAnimation.h>

namespace Compositing {

// Use only the most recent steps to find the velocity at the lift.
static constexpr AK::Duration velocity_sampling_window = AK::Duration::from_milliseconds(100);

// If the fingers stop for longer than this before they lift, no fling starts.
static constexpr AK::Duration maximum_pause_before_lift = AK::Duration::from_milliseconds(100);

static constexpr double minimum_fling_start_speed = 50;
static constexpr double minimum_fling_speed = 20;
static constexpr double maximum_fling_speed = 20000;

// A boost occurs only if the stopped fling was fast and a fast flick in the same direction starts soon after.
static constexpr double minimum_boosted_fling_speed = 350;
static constexpr double minimum_boosting_flick_speed = 150;
static constexpr AK::Duration maximum_boost_delay = AK::Duration::from_milliseconds(500);

static double speed_of(Gfx::FloatPoint velocity)
{
    return AK::hypot(static_cast<double>(velocity.x()), static_cast<double>(velocity.y()));
}

static Gfx::FloatPoint with_speed_capped(Gfx::FloatPoint velocity)
{
    auto speed = speed_of(velocity);
    if (speed <= maximum_fling_speed)
        return velocity;
    return velocity.scaled(static_cast<float>(maximum_fling_speed / speed));
}

void ScrollVelocityTracker::add_step(Gfx::FloatPoint delta, MonotonicTime time)
{
    m_steps.enqueue(Step { delta, time });
}

Optional<Gfx::FloatPoint> ScrollVelocityTracker::velocity_at_lift(MonotonicTime lift_time) const
{
    if (m_steps.is_empty())
        return {};
    auto last_step_time = m_steps.last().time;
    if (lift_time - last_step_time > maximum_pause_before_lift)
        return {};

    // Steps from before the window are not used. After a pause, they would make the velocity much too low.
    size_t first_index = m_steps.size() - 1;
    while (first_index > 0 && last_step_time - m_steps.at(first_index - 1).time <= velocity_sampling_window)
        --first_index;
    auto point_count = m_steps.size() - first_index;
    if (point_count < 2)
        return {};

    // Fit a least-squares line to the total displacement over time. Thus, one late or early step has less effect on
    // the velocity.
    // Times are relative to the last step to keep the sums small.
    double sum_t = 0;
    double sum_tt = 0;
    double sum_x = 0;
    double sum_y = 0;
    double sum_tx = 0;
    double sum_ty = 0;
    double x = 0;
    double y = 0;
    for (size_t i = first_index; i < m_steps.size(); ++i) {
        auto const& step = m_steps.at(i);
        x += step.delta.x();
        y += step.delta.y();
        auto t = (step.time - last_step_time).to_seconds_f64();
        sum_t += t;
        sum_tt += t * t;
        sum_x += x;
        sum_y += y;
        sum_tx += t * x;
        sum_ty += t * y;
    }

    auto n = static_cast<double>(point_count);
    auto determinant = n * sum_tt - sum_t * sum_t;
    // If all steps have the same time, there is no velocity.
    if (determinant <= 0)
        return {};
    return Gfx::FloatPoint {
        static_cast<float>((n * sum_tx - sum_t * sum_x) / determinant),
        static_cast<float>((n * sum_ty - sum_t * sum_y) / determinant),
    };
}

Optional<ScrollFling> ScrollFling::start(Gfx::FloatPoint velocity, MonotonicTime now)
{
    if (speed_of(velocity) < minimum_fling_start_speed)
        return {};
    return ScrollFling { with_speed_capped(velocity), now };
}

ScrollFling::ScrollFling(Gfx::FloatPoint velocity, MonotonicTime now)
    : m_initial_velocity(velocity)
    , m_initial_speed(speed_of(velocity))
    , m_started_at(now)
{
}

// The fraction of the initial speed that remains after the given time.
double ScrollFling::decay_at(double elapsed_seconds) const
{
    return AK::pow(momentum_distance_share_per_frame, elapsed_seconds / momentum_frame_duration_in_seconds);
}

Gfx::FloatPoint ScrollFling::velocity_at(MonotonicTime time) const
{
    if (m_finished)
        return {};
    auto elapsed_seconds = max(0.0, (time - m_started_at).to_seconds_f64());
    return m_initial_velocity.scaled(static_cast<float>(decay_at(elapsed_seconds)));
}

Optional<Gfx::FloatPoint> ScrollFling::step(MonotonicTime now)
{
    if (m_finished)
        return {};

    auto elapsed_seconds = min((now - m_started_at).to_seconds_f64(), maximum_momentum_duration_in_seconds);
    if (elapsed_seconds <= m_last_step_elapsed_seconds)
        return Gfx::FloatPoint {};

    // If v(t) = v0 * decay(t), the distance at time t is v0 * time_constant * (1 - decay(t)).
    static double const time_constant = momentum_frame_duration_in_seconds / -AK::log(momentum_distance_share_per_frame);
    auto decay = decay_at(elapsed_seconds);
    auto displacement = m_initial_velocity.scaled(static_cast<float>(time_constant * (m_last_step_decay - decay)));
    m_last_step_elapsed_seconds = elapsed_seconds;
    m_last_step_decay = decay;

    if (m_initial_speed * decay < minimum_fling_speed || elapsed_seconds >= maximum_momentum_duration_in_seconds)
        m_finished = true;
    return displacement;
}

void ScrollFlingBooster::fling_was_interrupted(Gfx::FloatPoint velocity, MonotonicTime now)
{
    if (speed_of(velocity) < minimum_boosted_fling_speed) {
        m_interrupted_fling.clear();
        return;
    }
    m_interrupted_fling = InterruptedFling { velocity, now };
}

Gfx::FloatPoint ScrollFlingBooster::boosted_velocity(Gfx::FloatPoint velocity, MonotonicTime now)
{
    auto interrupted_fling = m_interrupted_fling;
    m_interrupted_fling.clear();
    if (!interrupted_fling.has_value())
        return velocity;
    if (now - interrupted_fling->interrupted_at > maximum_boost_delay)
        return velocity;
    if (speed_of(velocity) < minimum_boosting_flick_speed)
        return velocity;
    auto direction_agreement = velocity.x() * interrupted_fling->velocity.x() + velocity.y() * interrupted_fling->velocity.y();
    if (direction_agreement <= 0)
        return velocity;
    return with_speed_capped(velocity + interrupted_fling->velocity);
}

}
