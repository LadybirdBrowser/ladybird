/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <compare>

namespace JS {

struct Position {
    u32 line { 0 };
    u32 column { 0 };

    auto operator<=>(Position const&) const = default;
};

}
