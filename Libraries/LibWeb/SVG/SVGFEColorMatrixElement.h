/*
 * Copyright (c) 2025, Pavel Shliak <shlyakpavel@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>
#include <LibWeb/SVG/SVGElement.h>
#include <LibWeb/SVG/SVGFilterPrimitiveStandardAttributes.h>

namespace Web::SVG {

// https://www.w3.org/TR/filter-effects-1/#InterfaceSVGFEColorMatrixElement
class SVGFEColorMatrixElement final
    : public SVGElement
    , public SVGFilterPrimitiveStandardAttributes<SVGFEColorMatrixElement> {
    WEB_WRAPPABLE(SVGFEColorMatrixElement, SVGElement);
    GC_DECLARE_ALLOCATOR(SVGFEColorMatrixElement);

public:
    virtual ~SVGFEColorMatrixElement() override = default;

    static constexpr unsigned short SVG_FECOLORMATRIX_TYPE_UNKNOWN = 0;
    static constexpr unsigned short SVG_FECOLORMATRIX_TYPE_MATRIX = 1;
    static constexpr unsigned short SVG_FECOLORMATRIX_TYPE_SATURATE = 2;
    static constexpr unsigned short SVG_FECOLORMATRIX_TYPE_HUEROTATE = 3;
    static constexpr unsigned short SVG_FECOLORMATRIX_TYPE_LUMINANCETOALPHA = 4;

    // IDL attributes
    GC::Ref<SVGAnimatedString> in1();
    GC::Ref<SVGAnimatedEnumeration> type() const;
    GC::Ref<SVGAnimatedNumberList> values();

private:
    SVGFEColorMatrixElement(DOM::Document&, DOM::QualifiedName);

    virtual void visit_edges(Cell::Visitor&) override;

    virtual void attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_) override;

    GC::Ptr<SVGAnimatedString> m_in1;
    GC::Ptr<SVGAnimatedNumberList> m_values;
};

}
