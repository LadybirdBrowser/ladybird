/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Set.h>
#include <LibJS/Runtime/SetIterator.h>
#include <LibWeb/Bindings/FontFaceSetPrototype.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/CSS/FontFace.h>
#include <LibWeb/DOM/EventTarget.h>

namespace Web::CSS {

struct FontSizeAdjust {
    bool is_from_font() const { return font_metric.has_value() && !number.has_value(); }
    bool is_none() const { return !number.has_value() && !font_metric.has_value(); }

    static FontSizeAdjust none() { return FontSizeAdjust {}; }
    Optional<FontMetric> font_metric;
    Optional<double> number;
};

}
