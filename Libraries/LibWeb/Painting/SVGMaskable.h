/*
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Forward.h>
#include <LibWeb/Forward.h>
#include <LibWeb/PixelUnits.h>

#pragma once

namespace Web::Painting {

class SVGMaskable {
public:
    virtual ~SVGMaskable() = default;

    virtual GC::Ptr<DOM::Node const> dom_node_of_svg() const = 0;

    // For <mask> element
    Optional<CSSPixelRect> get_svg_mask_area() const;
    Optional<Gfx::MaskKind> get_svg_mask_type() const;
    RefPtr<DisplayList> calculate_svg_mask_display_list(DisplayListRecordingContext&, CSSPixelRect const& mask_area) const;

    // For <clipPath> element
    Optional<CSSPixelRect> get_svg_clip_area() const;
    RefPtr<DisplayList> calculate_svg_clip_display_list(DisplayListRecordingContext&, CSSPixelRect const& clip_area) const;

private:
    Gfx::AffineTransform target_svg_transform() const;
};

}
