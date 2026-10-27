/*
 * Copyright (c) 2025, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>

namespace Web::CSS {

struct NumericRange {
    double min;
    double max;

    bool contains(double value) const { return value >= min && value <= max; }
};

using NumericRangesByValueType = HashMap<ValueType, NumericRange>;

constexpr NumericRange infinite_range = { AK::NumericLimits<float>::lowest(), AK::NumericLimits<float>::max() };

}
