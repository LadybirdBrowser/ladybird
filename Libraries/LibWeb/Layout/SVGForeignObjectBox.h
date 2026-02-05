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
    GC_CELL(SVGForeignObjectBox, BlockContainer);
    GC_DECLARE_ALLOCATOR(SVGForeignObjectBox);

public:
    SVGForeignObjectBox(DOM::Document&, SVG::SVGForeignObjectElement&, GC::Ref<CSS::ComputedProperties>);
    virtual ~SVGForeignObjectBox() override = default;

    SVG::SVGForeignObjectElement& dom_node() { return static_cast<SVG::SVGForeignObjectElement&>(*BlockContainer::dom_node()); }
    SVG::SVGForeignObjectElement const& dom_node() const { return static_cast<SVG::SVGForeignObjectElement const&>(*BlockContainer::dom_node()); }

    virtual GC::Ptr<Painting::Paintable> create_paintable() const override;

private:
    virtual bool is_svg_foreign_object_box() const override { return true; }
};

template<>
inline bool Node::fast_is<SVGForeignObjectBox>() const { return is_svg_foreign_object_box(); }

}
