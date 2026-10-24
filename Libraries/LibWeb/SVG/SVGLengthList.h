/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
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
    [[nodiscard]] static GC::Ref<SVGLengthList> create(GC::Ref<SVGElement>, Utf16FlyString reflected_attribute, SVGLength::Directionality, ReadOnlyList);
    virtual ~SVGLengthList() override = default;

    // https://svgwg.org/svg2-draft/types.html#TermSynchronizeList
    void synchronize();

    // https://svgwg.org/svg2-draft/types.html#TermReserialize
    virtual void reserialize_reflected_attribute() override;

private:
    SVGLengthList(GC::Ref<SVGElement>, Utf16FlyString reflected_attribute, SVGLength::Directionality, ReadOnlyList);

    virtual void visit_edges(Visitor&) override;

    virtual void attach(GC::Ref<SVGLength> const&) override;
    virtual void detach(GC::Ref<SVGLength> const&) override;
    virtual GC::Ref<SVGLength> copy_if_not_detached(GC::Ref<SVGLength>) override;

    GC::Ref<SVGElement> m_element;
    Utf16FlyString m_reflected_attribute;
    SVGLength::Directionality m_directionality;
    bool m_is_reserializing { false };
};

}
