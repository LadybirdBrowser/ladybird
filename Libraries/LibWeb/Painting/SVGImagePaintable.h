/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/Box.h>
#include <LibWeb/Painting/SVGGraphicsPaintable.h>

namespace Web::Painting {

class SVGImagePaintable final : public SVGGraphicsPaintable {
public:
    static NonnullRefPtr<SVGImagePaintable> create(Layout::Box const&);
    virtual StringView class_name() const override { return "SVGImagePaintable"sv; }

private:
    SVGImagePaintable(Layout::Box const&);
};

}
