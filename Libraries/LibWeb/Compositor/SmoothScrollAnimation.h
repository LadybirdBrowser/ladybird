/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Time.h>
#include <LibGfx/Point.h>
#include <LibWeb/Export.h>

namespace Web::Compositor {

class WEB_API SmoothScrollAnimation {
public:
    struct Sample {
        Gfx::FloatPoint offset;
        bool complete { false };
    };

    SmoothScrollAnimation(Gfx::FloatPoint start_offset, Gfx::FloatPoint destination_offset, double pixels_per_css_pixel);

    AK::Duration duration() const { return m_duration; }
    Sample sample(AK::Duration elapsed) const;

private:
    Gfx::FloatPoint m_start_offset;
    Gfx::FloatPoint m_destination_offset;
    AK::Duration m_duration;
};

}
