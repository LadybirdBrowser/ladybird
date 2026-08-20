/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/Box.h>
#include <LibWeb/Painting/Paintable.h>

namespace Web::Painting {

class SVGSVGPaintable final : public Paintable {
public:
    static NonnullRefPtr<SVGSVGPaintable> create(Layout::Box const&);
    virtual StringView class_name() const override { return "SVGSVGPaintable"sv; }

    CSSPixelSize svg_viewport_size() const
    {
        return {
            CSSPixels::from_raw(rust_data().svg_viewport_size.width),
            CSSPixels::from_raw(rust_data().svg_viewport_size.height),
        };
    }

protected:
    SVGSVGPaintable(Layout::Box const&);

private:
    virtual bool is_svg_svg_paintable() const final { return true; }
};

template<>
inline bool Paintable::fast_is<SVGSVGPaintable>() const { return is_svg_svg_paintable(); }

}
