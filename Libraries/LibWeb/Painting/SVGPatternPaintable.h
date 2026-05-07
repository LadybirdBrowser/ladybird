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
public:
    static NonnullRefPtr<SVGPatternPaintable> create(Layout::SVGPatternBox const&);
    virtual StringView class_name() const override { return "SVGPatternPaintable"sv; }

    bool forms_unconnected_subtree() const override
    {
        return true;
    }

protected:
    SVGPatternPaintable(Layout::SVGPatternBox const&);
};

}
