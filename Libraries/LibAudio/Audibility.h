/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace Audio {

// Whether the AudioServer has recently heard something, muted or not.
enum class Audibility : u8 {
    Inaudible,
    Audible,
};

}
