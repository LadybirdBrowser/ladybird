/*
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Forward.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Painting/DisplayListResourceStorage.h>
#include <LibWeb/PixelUnits.h>

namespace Web::Painting {

class SVGMaskable {
public:
    virtual ~SVGMaskable() = default;

    virtual GC::Ptr<DOM::Node const> dom_node_of_svg() const = 0;

    // For <mask> element
    Optional<CSSPixelRect> get_svg_mask_area() const;
    Optional<Gfx::MaskKind> get_svg_mask_type() const;

    // For <clipPath> element
    Optional<CSSPixelRect> get_svg_clip_area() const;

private:
    Gfx::AffineTransform object_bounding_box_content_units_transform() const;
};

}
