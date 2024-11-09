/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace JS {

struct Position {
    size_t line { 0 };
    size_t column { 0 };
    size_t offset { 0 };
};

}
