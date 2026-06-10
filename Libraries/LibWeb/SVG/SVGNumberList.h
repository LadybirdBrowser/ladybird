/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/SVG/SVGList.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::SVG {

// https://www.w3.org/TR/SVG2/types.html#InterfaceSVGNumberList
class SVGNumberList final
    : public Bindings::Wrappable
    , public SVGList<GC::Ref<SVGNumber>> {
    WEB_WRAPPABLE(SVGNumberList, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(SVGNumberList);

public:
    [[nodiscard]] static GC::Ref<SVGNumberList> create(Vector<GC::Ref<SVGNumber>>, ReadOnlyList);
    [[nodiscard]] static GC::Ref<SVGNumberList> create(ReadOnlyList);
    virtual ~SVGNumberList() override = default;

private:
    SVGNumberList(Vector<GC::Ref<SVGNumber>>, ReadOnlyList);
    explicit SVGNumberList(ReadOnlyList);

    virtual void visit_edges(Visitor&) override;
};

}
