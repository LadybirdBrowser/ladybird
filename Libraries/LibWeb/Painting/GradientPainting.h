/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>
#include <LibWeb/Painting/GradientData.h>

namespace Web::Painting {

LinearGradientData resolve_linear_gradient_data(Layout::NodeWithStyle const&, CSSPixelSize, CSS::LinearGradientStyleValue const&);
ConicGradientData resolve_conic_gradient_data(Layout::NodeWithStyle const&, CSS::ConicGradientStyleValue const&);
RadialGradientData resolve_radial_gradient_data(Layout::NodeWithStyle const&, CSSPixelSize, CSS::RadialGradientStyleValue const&);

}
