/*
 * Copyright (c) 2024-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16View.h>

namespace Web::CSS {

enum class PreferredMotion {
    Auto,
    NoPreference,
    Reduce,
};

PreferredMotion preferred_motion_from_string(Utf16View);

}
