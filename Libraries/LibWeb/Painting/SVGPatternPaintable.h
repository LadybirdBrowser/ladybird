/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/SVGPatternBox.h>
#include <LibWeb/Painting/SVGPaintable.h>

namespace Web::Painting {

class SVGPatternPaintable : public SVGPaintable {
    GC_CELL(SVGPatternPaintable, SVGPaintable);
    GC_DECLARE_ALLOCATOR(SVGPatternPaintable);

public:
    static GC::Ref<SVGPatternPaintable> create(Layout::SVGPatternBox const&);

    bool forms_unconnected_subtree() const override
    {
        return true;
    }

protected:
    SVGPatternPaintable(Layout::SVGPatternBox const&);
};

}
