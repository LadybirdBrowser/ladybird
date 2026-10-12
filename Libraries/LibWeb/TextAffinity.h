/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace Web {

// Disambiguates the visual position of a text offset at soft wraps and bidirectional run boundaries. Upstream uses
// the edge of the preceding logical text; Downstream uses the edge of the following logical text.
enum class TextAffinity : u8 {
    Downstream,
    Upstream,
};

}
