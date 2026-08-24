/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace Web::CSS {

// Corresponds to Kleene 3-valued logic.
enum class MatchResult : u8 {
    False,
    True,
    Unknown,
};

}
