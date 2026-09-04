/*
 * Copyright (c) 2024, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/HeapVector.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/SVG/SVGList.h>
#include <LibWeb/SVG/SVGTransform.h>

namespace Web::SVG {

// https://svgwg.org/svg2-draft/single-page.html#coords-InterfaceSVGTransformList
class SVGTransformList final
    : public Bindings::GCAllocatedWrappable
    , public SVGList<GC::Ref<SVGTransform>> {
    WEB_WRAPPABLE(SVGTransformList, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(SVGTransformList);

public:
    using List = GC::HeapVector<GC::Ref<SVGTransform>>;

    [[nodiscard]] static GC::Ref<SVGTransformList> create(GC::Ref<List>, ReadOnlyList);
    [[nodiscard]] static GC::Ref<SVGTransformList> create(ReadOnlyList);
    virtual ~SVGTransformList() override = default;

private:
    SVGTransformList(GC::Ref<List>, ReadOnlyList);
    explicit SVGTransformList(ReadOnlyList);

    virtual void visit_edges(Visitor&) override;
};

}
