/*
 * Copyright (c) 2024, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/SVGClipBox.h>
#include <LibWeb/Painting/SVGPaintable.h>

namespace Web::Painting {

class SVGClipPaintable : public SVGPaintable {
    GC_CELL(SVGClipPaintable, SVGPaintable);
    GC_DECLARE_ALLOCATOR(SVGClipPaintable);

public:
    static GC::Ref<SVGClipPaintable> create(Layout::SVGClipBox const&);

    bool forms_unconnected_subtree() const override
    {
        return true;
    }

protected:
    SVGClipPaintable(Layout::SVGClipBox const&);
};

}
