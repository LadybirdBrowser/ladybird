/*
 * Copyright (c) 2025, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/Length.h>

namespace Web::CSS {

struct TreeCountingFunctionResolutionContext {
    size_t sibling_count;
    size_t sibling_index;
};

struct ComputationContext {
    Length::ResolutionContext length_resolution_context;
    Optional<TreeCountingFunctionResolutionContext> tree_counting_function_resolution_context {};
};

}
