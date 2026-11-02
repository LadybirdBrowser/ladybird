/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/EnumBits.h>
#include <AK/Types.h>

namespace Web::CSS {

// What installing an element's style record has the main thread do beside swapping its style, as the style engine's
// MainEffects names it.
enum class MainEffects : u32 {
    None = 0,
    Animated = 1 << 0,
    DisplayNoneFlip = 1 << 1,
    ClearHiddenDescendants = 1 << 2,
    VisibilityFlip = 1 << 3,
    CustomProperties = 1 << 4,
    AnchorNames = 1 << 5,
    RootFontMetrics = 1 << 6,
    RevealWaiting = 1 << 7,
    SvgReferences = 1 << 8,
    Timelines = 1 << 9,
    FormControl = 1 << 10,
    ScrollSnap = 1 << 11,
    Rebuild = 1 << 12,
    ContainerEffects = 1 << 13,
    Images = 1 << 14,
    Highlights = 1 << 15,
    HostBoxFacts = 1 << 16,
    AnimationPlan = 1 << 17,
    TransitionStep = 1 << 18,
};

AK_ENUM_BITWISE_OPERATORS(MainEffects);

}
