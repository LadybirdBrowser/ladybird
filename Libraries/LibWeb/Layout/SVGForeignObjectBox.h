/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibWeb/Layout/BlockContainer.h>
#include <LibWeb/Layout/SVGGraphicsBox.h>
#include <LibWeb/SVG/SVGForeignObjectElement.h>

namespace Web::Layout {

class SVGForeignObjectBox final : public BlockContainer {
    JS_CELL(SVGForeignObjectBox, BlockContainer);
    JS_DECLARE_ALLOCATOR(SVGForeignObjectBox);

public:
    SVGForeignObjectBox(DOM::Document&, SVG::SVGForeignObjectElement&, CSS::StyleProperties);
    virtual ~SVGForeignObjectBox() override = default;

    SVG::SVGForeignObjectElement& dom_node() { return static_cast<SVG::SVGForeignObjectElement&>(*BlockContainer::dom_node()); }
    SVG::SVGForeignObjectElement const& dom_node() const { return static_cast<SVG::SVGForeignObjectElement const&>(*BlockContainer::dom_node()); }

    virtual JS::GCPtr<Painting::Paintable> create_paintable() const override;
};

}
