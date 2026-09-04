/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/HeapVector.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/SVG/SVGList.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::SVG {

// https://www.w3.org/TR/SVG2/types.html#InterfaceSVGNumberList
class SVGNumberList final
    : public Bindings::GCAllocatedWrappable
    , public SVGList<GC::Ref<SVGNumber>> {
    WEB_WRAPPABLE(SVGNumberList, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(SVGNumberList);

public:
    using List = GC::HeapVector<GC::Ref<SVGNumber>>;

    [[nodiscard]] static GC::Ref<SVGNumberList> create(GC::Ref<List>, ReadOnlyList);
    [[nodiscard]] static GC::Ref<SVGNumberList> create(ReadOnlyList);
    virtual ~SVGNumberList() override = default;

private:
    SVGNumberList(GC::Ref<List>, ReadOnlyList);
    explicit SVGNumberList(ReadOnlyList);

    virtual void visit_edges(Visitor&) override;
};

}
