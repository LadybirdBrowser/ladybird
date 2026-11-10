/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Time.h>
#include <LibCompositing/Export.h>
#include <LibCompositing/Types.h>
#include <LibGfx/Point.h>

namespace Compositing {

namespace RustFFI {

struct SmoothScrollAnimation;

}

class COMPOSITING_API SmoothScrollAnimation {
public:
    struct Sample {
        Gfx::FloatPoint offset;
        bool complete { false };
    };

    SmoothScrollAnimation(Gfx::FloatPoint start_offset, Gfx::FloatPoint destination_offset, double pixels_per_css_pixel, ScrollAnimationKind = ScrollAnimationKind::SmoothScroll);

    ~SmoothScrollAnimation();
    SmoothScrollAnimation(SmoothScrollAnimation&&);
    SmoothScrollAnimation& operator=(SmoothScrollAnimation&&);
    SmoothScrollAnimation(SmoothScrollAnimation const&) = delete;
    SmoothScrollAnimation& operator=(SmoothScrollAnimation const&) = delete;

    ScrollAnimationKind kind() const;
    Sample sample(AK::Duration elapsed) const;
    void retarget(Gfx::FloatPoint destination_offset, AK::Duration elapsed);

private:
    RustFFI::SmoothScrollAnimation* m_animation;
};

}
