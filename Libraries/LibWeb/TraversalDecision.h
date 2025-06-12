/*
 * Copyright (c) 2024, Tim Ledbetter <timledbetter@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace Web {

enum class TraversalDecision {
    Continue,
    SkipChildrenAndContinue,
    Break,
};

}
