/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/SVGImageBox.h>
#include <LibWeb/Painting/SVGGraphicsPaintable.h>

namespace Web::Painting {

class SVGImagePaintable final : public SVGGraphicsPaintable {
public:
    static NonnullRefPtr<SVGImagePaintable> create(Layout::SVGImageBox const&);
    virtual StringView class_name() const override { return "SVGImagePaintable"sv; }

    virtual void paint(DisplayListRecordingContext&, PaintPhase) const override;

    Layout::SVGImageBox const& layout_box() const;

private:
    SVGImagePaintable(Layout::SVGImageBox const&);
};

}
