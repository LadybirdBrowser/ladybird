/*
 * Copyright (c) 2025, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "TreeCountingFunctionStyleValue.h"

namespace Web::CSS {

// The function discriminant crosses the style value FFI as a raw code; the Rust serializer
// depends on it.
static_assert(to_underlying(TreeCountingFunctionStyleValue::TreeCountingFunction::SiblingCount) == 0);
static_assert(to_underlying(TreeCountingFunctionStyleValue::TreeCountingFunction::SiblingIndex) == 1);

}
