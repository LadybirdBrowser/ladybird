/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace Media {

// Whether a seek of a media pipeline node should ignore data it holds, as that data has been replaced.
enum class InvalidateHeldData : u8 {
    No,
    Yes,
};

}
