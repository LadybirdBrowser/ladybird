/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/SVG/SVGList.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::SVG {

// https://www.w3.org/TR/SVG2/types.html#InterfaceSVGNumberList
class SVGNumberList final
    : public Bindings::PlatformObject
    , public SVGList<GC::Ref<SVGNumber>> {
    WEB_PLATFORM_OBJECT(SVGNumberList, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(SVGNumberList);

public:
    [[nodiscard]] static GC::Ref<SVGNumberList> create(JS::Realm&, Vector<GC::Ref<SVGNumber>>, ReadOnlyList);
    [[nodiscard]] static GC::Ref<SVGNumberList> create(JS::Realm&, ReadOnlyList);
    virtual ~SVGNumberList() override = default;

private:
    SVGNumberList(JS::Realm&, Vector<GC::Ref<SVGNumber>>, ReadOnlyList);
    SVGNumberList(JS::Realm&, ReadOnlyList);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor&) override;
};

}
