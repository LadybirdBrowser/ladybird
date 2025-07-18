/*
 * Copyright (c) 2025, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/Angle.h>
#include <LibWeb/CSS/Frequency.h>
#include <LibWeb/CSS/Length.h>
#include <LibWeb/CSS/Time.h>

namespace Web::CSS {

struct CalculationResolutionContext {
    Variant<Empty, Angle, Frequency, Length, Time> percentage_basis {};
    Optional<Length::ResolutionContext> length_resolution_context;
};

}
