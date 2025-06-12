/*
 * Copyright (c) 2024, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringView.h>

namespace Web::CSS {

enum class PreferredMotion {
    Auto,
    NoPreference,
    Reduce,
};

PreferredMotion preferred_motion_from_string(StringView);
StringView preferred_motion_to_string(PreferredMotion);

}
