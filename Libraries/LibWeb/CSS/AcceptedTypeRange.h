/*
 * Copyright (c) 2025, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>

namespace Web::CSS {

struct AcceptedTypeRange {
    double min;
    double max;
};
using AcceptedTypeRangeMap = HashMap<ValueType, AcceptedTypeRange>;

}
