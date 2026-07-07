/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace Web {

// Disambiguates the visual position of a text offset at a soft wrap boundary, where the same offset can render both
// at the end of one line and at the start of the next: Upstream renders at the end of the earlier line, Downstream
// at the start of the later line. For all other offsets, affinity has no effect.
enum class TextAffinity : u8 {
    Downstream,
    Upstream,
};

}
