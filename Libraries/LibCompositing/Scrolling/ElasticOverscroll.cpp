/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibCompositing/Scrolling/ElasticOverscroll.h>

namespace Compositing {

// The stretch is the overscroll divided by the stiffness, and a released stretch S0 with velocity V0 is
// (S0 + amplitude * V0 * t) * exp(-t * stiffness / period) after t seconds.
static constexpr double rubber_band_stiffness = 20;
static constexpr double rubber_band_amplitude = 0.31;
static constexpr double rubber_band_period = 1.6;
static constexpr double rebound_decay_per_second = rubber_band_stiffness / rubber_band_period;

static constexpr double largest_bounce_as_share_of_scrollport = 0.2;
static constexpr float stretch_that_counts_as_sprung_back = 1;

// A scroller released at its edge with speed V peaks at amplitude * V / (decay * e). Wheel events carry no
// timestamps, so the speed measured for events that were handled in a burst can be far above any real one.
static float fastest_release_speed_for(float scrollport_length)
{
    return static_cast<float>(largest_bounce_as_share_of_scrollport * scrollport_length * rebound_decay_per_second * AK::E<double> / rubber_band_amplitude);
}

ElasticOverscroll::ElasticOverscroll(Gfx::FloatSize scrollport_size)
    : m_scrollport_size(scrollport_size)
{
}

Gfx::FloatPoint ElasticOverscroll::stretch() const
{
    auto unrounded_stretch = m_rebound.has_value()
        ? m_rebound->current_stretch
        : m_accumulated_overscroll.scaled(static_cast<float>(1 / rubber_band_stiffness));
    return { AK::trunc(unrounded_stretch.x()), AK::trunc(unrounded_stretch.y()) };
}

static float relax_axis_by(float& accumulated_overscroll, float delta)
{
    bool delta_points_back_to_scroll_range = (accumulated_overscroll > 0 && delta < 0) || (accumulated_overscroll < 0 && delta > 0);
    if (!delta_points_back_to_scroll_range)
        return delta;
    if (AK::abs(delta) <= AK::abs(accumulated_overscroll)) {
        accumulated_overscroll += delta;
        return 0;
    }
    auto delta_left_to_scroll_with = delta + accumulated_overscroll;
    accumulated_overscroll = 0;
    return delta_left_to_scroll_with;
}

Gfx::FloatPoint ElasticOverscroll::relax_by(Gfx::FloatPoint delta)
{
    if (m_rebound.has_value()) {
        m_accumulated_overscroll = m_rebound->current_stretch.scaled(static_cast<float>(rubber_band_stiffness));
        m_rebound.clear();
    }

    auto accumulated_overscroll_x = m_accumulated_overscroll.x();
    auto accumulated_overscroll_y = m_accumulated_overscroll.y();
    Gfx::FloatPoint delta_left_to_scroll_with {
        relax_axis_by(accumulated_overscroll_x, delta.x()),
        relax_axis_by(accumulated_overscroll_y, delta.y()),
    };
    m_accumulated_overscroll = { accumulated_overscroll_x, accumulated_overscroll_y };
    return delta_left_to_scroll_with;
}

void ElasticOverscroll::pull_by(Gfx::FloatPoint overscroll_delta)
{
    VERIFY(!m_rebound.has_value());
    auto largest_overscroll_x = m_scrollport_size.width() * static_cast<float>(rubber_band_stiffness);
    auto largest_overscroll_y = m_scrollport_size.height() * static_cast<float>(rubber_band_stiffness);
    m_accumulated_overscroll = {
        clamp(m_accumulated_overscroll.x() + overscroll_delta.x(), -largest_overscroll_x, largest_overscroll_x),
        clamp(m_accumulated_overscroll.y() + overscroll_delta.y(), -largest_overscroll_y, largest_overscroll_y),
    };
}

void ElasticOverscroll::release(Gfx::FloatPoint velocity, MonotonicTime now)
{
    if (m_rebound.has_value())
        return;

    auto initial_stretch = m_accumulated_overscroll.scaled(static_cast<float>(1 / rubber_band_stiffness));
    m_accumulated_overscroll = {};

    // Only an axis that is stretched is at its edge. The other one must not bounce off an edge it is not at.
    auto release_velocity_for_axis = [](float stretch, float velocity, float scrollport_length) -> float {
        if (stretch == 0)
            return 0;
        auto fastest_release_speed = fastest_release_speed_for(scrollport_length);
        return clamp(velocity, -fastest_release_speed, fastest_release_speed);
    };
    m_rebound = Rebound {
        .started_at = now,
        .initial_stretch = initial_stretch,
        .initial_velocity = {
            release_velocity_for_axis(initial_stretch.x(), velocity.x(), m_scrollport_size.width()),
            release_velocity_for_axis(initial_stretch.y(), velocity.y(), m_scrollport_size.height()),
        },
        .current_stretch = initial_stretch,
    };
}

struct ReboundAxisSample {
    float stretch { 0 };
    bool has_sprung_back { true };
};

static ReboundAxisSample sample_rebound_axis(float initial_stretch, float initial_velocity, float scrollport_length, double elapsed_seconds)
{
    if (initial_stretch == 0)
        return {};

    auto stretch_before_decay = initial_stretch + rubber_band_amplitude * initial_velocity * elapsed_seconds;
    // A release velocity that points back to the scroll range must not carry the content past the edge it left.
    bool crossed_the_edge = (stretch_before_decay > 0) != (initial_stretch > 0);
    if (crossed_the_edge)
        return {};

    auto stretch = static_cast<float>(stretch_before_decay * AK::exp(-elapsed_seconds * rebound_decay_per_second));
    stretch = clamp(stretch, -scrollport_length, scrollport_length);

    // A bounce starts with almost no stretch, so a small stretch only means the rebound is over once it shrinks.
    auto stretch_growth_before_decay = rubber_band_amplitude * initial_velocity - rebound_decay_per_second * stretch_before_decay;
    bool is_still_stretching = stretch_before_decay * stretch_growth_before_decay > 0;
    return {
        .stretch = stretch,
        .has_sprung_back = !is_still_stretching && AK::abs(stretch) < stretch_that_counts_as_sprung_back,
    };
}

bool ElasticOverscroll::advance_rebound_to(MonotonicTime frame_time)
{
    if (!m_rebound.has_value())
        return false;

    // The time of a display tick can be earlier than the event that released the stretch.
    auto elapsed_seconds = max(0.0, (frame_time - m_rebound->started_at).to_seconds_f64());
    auto horizontal = sample_rebound_axis(m_rebound->initial_stretch.x(), m_rebound->initial_velocity.x(), m_scrollport_size.width(), elapsed_seconds);
    auto vertical = sample_rebound_axis(m_rebound->initial_stretch.y(), m_rebound->initial_velocity.y(), m_scrollport_size.height(), elapsed_seconds);
    if (horizontal.has_sprung_back && vertical.has_sprung_back) {
        m_rebound.clear();
        return false;
    }
    m_rebound->current_stretch = {
        horizontal.has_sprung_back ? 0 : horizontal.stretch,
        vertical.has_sprung_back ? 0 : vertical.stretch,
    };
    return true;
}

}
