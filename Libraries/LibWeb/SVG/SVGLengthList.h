/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/HeapVector.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/SVG/SVGLength.h>
#include <LibWeb/SVG/SVGList.h>

namespace Web::SVG {

// https://svgwg.org/svg2-draft/types.html#InterfaceSVGLengthList
class SVGLengthList final
    : public Bindings::GCAllocatedWrappable
    , public SVGList<GC::Ref<SVGLength>> {
    WEB_WRAPPABLE(SVGLengthList, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(SVGLengthList);

public:
    using List = GC::HeapVector<GC::Ref<SVGLength>>;

    [[nodiscard]] static GC::Ref<SVGLengthList> create(GC::Ref<List>, ReadOnlyList);
    [[nodiscard]] static GC::Ref<SVGLengthList> create(ReadOnlyList);
    virtual ~SVGLengthList() override = default;

private:
    SVGLengthList(GC::Ref<List>, ReadOnlyList);
    explicit SVGLengthList(ReadOnlyList);

    virtual void visit_edges(Visitor&) override;
};

}
