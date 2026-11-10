/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCompositing/RustFFI.h>
#include <LibCompositing/Scrolling/SmoothScrollAnimation.h>

namespace Compositing {

SmoothScrollAnimation::SmoothScrollAnimation(Gfx::FloatPoint start_offset, Gfx::FloatPoint destination_offset, double pixels_per_css_pixel, ScrollAnimationKind kind)
    : m_animation(RustFFI::smooth_scroll_animation_create(start_offset, destination_offset, pixels_per_css_pixel, kind))
{
}

SmoothScrollAnimation::~SmoothScrollAnimation()
{
    RustFFI::smooth_scroll_animation_destroy(m_animation);
}

SmoothScrollAnimation::SmoothScrollAnimation(SmoothScrollAnimation&& other)
    : m_animation(exchange(other.m_animation, nullptr))
{
}

SmoothScrollAnimation& SmoothScrollAnimation::operator=(SmoothScrollAnimation&& other)
{
    if (this != &other) {
        RustFFI::smooth_scroll_animation_destroy(m_animation);
        m_animation = exchange(other.m_animation, nullptr);
    }
    return *this;
}

ScrollAnimationKind SmoothScrollAnimation::kind() const
{
    return RustFFI::smooth_scroll_animation_kind(m_animation);
}

SmoothScrollAnimation::Sample SmoothScrollAnimation::sample(AK::Duration elapsed) const
{
    Sample sample;
    sample.complete = RustFFI::smooth_scroll_animation_sample(m_animation, elapsed.to_nanoseconds(), &sample.offset);
    return sample;
}

void SmoothScrollAnimation::retarget(Gfx::FloatPoint destination_offset, AK::Duration elapsed)
{
    RustFFI::smooth_scroll_animation_retarget(m_animation, destination_offset, elapsed.to_nanoseconds());
}

}
