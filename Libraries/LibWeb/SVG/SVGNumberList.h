/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>
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
    [[nodiscard]] static GC::Ref<SVGNumberList> create(GC::Ref<SVGElement>, Utf16FlyString reflected_attribute, ReadOnlyList);
    virtual ~SVGNumberList() override = default;

    // https://svgwg.org/svg2-draft/types.html#TermSynchronizeList
    void synchronize();

    // https://svgwg.org/svg2-draft/types.html#TermReserialize
    virtual void reserialize_reflected_attribute() override;

private:
    SVGNumberList(GC::Ref<SVGElement>, Utf16FlyString reflected_attribute, ReadOnlyList);

    virtual void visit_edges(Visitor&) override;

    virtual void attach(GC::Ref<SVGNumber> const&) override;
    virtual void detach(GC::Ref<SVGNumber> const&) override;
    virtual GC::Ref<SVGNumber> copy_if_not_detached(GC::Ref<SVGNumber>) override;

    GC::Ref<SVGElement> m_element;
    Utf16FlyString m_reflected_attribute;
    bool m_is_reserializing { false };
};

}
