/*
 * Copyright (c) 2024, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/SVG/SVGList.h>
#include <LibWeb/SVG/SVGTransform.h>

namespace Web::SVG {

// https://svgwg.org/svg2-draft/coords.html#InterfaceSVGTransformList
class SVGTransformList final
    : public Bindings::GCAllocatedWrappable
    , public SVGList<GC::Ref<SVGTransform>> {
    WEB_WRAPPABLE(SVGTransformList, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(SVGTransformList);

public:
    [[nodiscard]] static GC::Ref<SVGTransformList> create(GC::Ref<SVGElement>, Utf16FlyString reflected_attribute, ReadOnlyList);
    virtual ~SVGTransformList() override = default;

    WebIDL::ExceptionOr<GC::Ref<SVGTransform>> create_svg_transform_from_matrix(Bindings::DOMMatrix2DInit const&);
    WebIDL::ExceptionOr<GC::Ptr<SVGTransform>> consolidate();

    // https://svgwg.org/svg2-draft/types.html#TermSynchronizeList
    void synchronize();

    // https://svgwg.org/svg2-draft/types.html#TermReserialize
    virtual void reserialize_reflected_attribute() override;

private:
    SVGTransformList(GC::Ref<SVGElement>, Utf16FlyString reflected_attribute, ReadOnlyList);

    virtual void visit_edges(Visitor&) override;

    virtual void attach(GC::Ref<SVGTransform> const&) override;
    virtual void detach(GC::Ref<SVGTransform> const&) override;
    virtual GC::Ref<SVGTransform> copy_if_not_detached(GC::Ref<SVGTransform>) override;

    GC::Ref<SVGElement> m_element;
    Utf16FlyString m_reflected_attribute;
    bool m_is_reserializing { false };
};

}
