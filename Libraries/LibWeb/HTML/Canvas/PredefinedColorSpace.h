/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/ColorStyleValue.h>
#include <LibWeb/Export.h>
#include <LibWebCommon/Bindings/PredefinedColorSpace.h>

namespace Web::HTML {

// The CSS color space of the predefined color space.
WEB_API CSS::ColorStyleValue::ColorType css_color_type_for(Bindings::PredefinedColorSpace);

// The predefined color space whose underlying value the Rust implementation passes.
WEB_API Bindings::PredefinedColorSpace predefined_color_space_from_underlying(u8);

}
