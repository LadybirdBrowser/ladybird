/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace Web::Painting {

struct DisplayListCommandRange {
    u32 offset { 0 };
    u32 size { 0 };

    [[nodiscard]] bool is_empty() const { return size == 0; }
};

}
