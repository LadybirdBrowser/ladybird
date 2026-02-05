/*
 * Copyright (c) 2024, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/SVGMaskBox.h>
#include <LibWeb/Painting/SVGGraphicsPaintable.h>

namespace Web::Painting {

class SVGMaskPaintable : public SVGGraphicsPaintable {
    GC_CELL(SVGMaskPaintable, SVGGraphicsPaintable);
    GC_DECLARE_ALLOCATOR(SVGMaskPaintable);

public:
    static GC::Ref<SVGMaskPaintable> create(Layout::SVGMaskBox const&);

    bool forms_unconnected_subtree() const override
    {
        // Masks should not be painted (i.e. reachable) unless referenced by another element.
        return true;
    }

protected:
    SVGMaskPaintable(Layout::SVGMaskBox const&);
};

}
