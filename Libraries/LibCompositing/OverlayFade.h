/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/StdLibExtras.h>
#include <AK/Time.h>

namespace Compositing {

// A fade that turns back from whatever opacity it has reached.
class OverlayFade {
public:
    static constexpr auto duration = AK::Duration::from_milliseconds(150);

    void fade_to(float target_opacity, MonotonicTime now)
    {
        if (target_opacity == m_target_opacity)
            return;
        m_initial_opacity = m_opacity;
        m_target_opacity = target_opacity;
        m_started_at = now;
    }

    // Ends any fade, with the opacity at once where it would end.
    void jump_to(float opacity)
    {
        m_opacity = opacity;
        m_initial_opacity = opacity;
        m_target_opacity = opacity;
        m_started_at.clear();
    }

    // Returns whether the opacity changed.
    bool advance_to(MonotonicTime frame_time)
    {
        auto opacity = opacity_at(frame_time);
        if (opacity == m_opacity)
            return false;
        m_opacity = opacity;
        return true;
    }

    float opacity() const { return m_opacity; }
    bool is_fading() const { return m_opacity != m_target_opacity; }

private:
    float opacity_at(MonotonicTime frame_time) const
    {
        if (!m_started_at.has_value())
            return m_target_opacity;

        auto progress = (frame_time - *m_started_at).to_seconds_f64() / duration.to_seconds_f64();
        if (progress >= 1)
            return m_target_opacity;
        progress = max(progress, 0.0);

        // Smoothstep, so that the fade starts and ends gently.
        auto eased_progress = static_cast<float>(progress * progress * (3 - 2 * progress));
        return m_initial_opacity + (m_target_opacity - m_initial_opacity) * eased_progress;
    }

    float m_opacity { 0 };
    float m_initial_opacity { 0 };
    float m_target_opacity { 0 };
    Optional<MonotonicTime> m_started_at;
};

}
