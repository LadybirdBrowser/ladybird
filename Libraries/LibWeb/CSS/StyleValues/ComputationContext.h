/*
 * Copyright (c) 2025, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/Length.h>

namespace Web::CSS {

struct ComputationContext {
    Length::ResolutionContext length_resolution_context;
};

}
