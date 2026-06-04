/*
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

struct SourcePosition {
    size_t line { 0 };
    size_t column { 0 };
};
