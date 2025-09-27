/*
 * Copyright (c) 2025, Pavel Shliak <shlyakpavel@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/SVGElement.h>
#include <LibWeb/SVG/SVGFilterPrimitiveStandardAttributes.h>

namespace Web::SVG {

class SVGAnimatedEnumeration;
class SVGAnimatedString;

// https://www.w3.org/TR/filter-effects-1/#InterfaceSVGFEColorMatrixElement
class SVGFEColorMatrixElement final
    : public SVGElement
    , public SVGFilterPrimitiveStandardAttributes<SVGFEColorMatrixElement> {
    WEB_PLATFORM_OBJECT(SVGFEColorMatrixElement, SVGElement);
    GC_DECLARE_ALLOCATOR(SVGFEColorMatrixElement);

public:
    virtual ~SVGFEColorMatrixElement() override = default;

    // IDL attributes
    GC::Ref<SVGAnimatedString> in1();
    GC::Ref<SVGAnimatedEnumeration> type() const;
    GC::Ref<SVGAnimatedString> values();

private:
    SVGFEColorMatrixElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;
    virtual void attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& new_value, Optional<FlyString> const& namespace_) override;

    GC::Ptr<SVGAnimatedString> m_in1;
    GC::Ptr<SVGAnimatedString> m_values;
};

}
