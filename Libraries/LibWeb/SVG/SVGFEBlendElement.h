/*
 * Copyright (c) 2025, Lucien Fiorini <lucienfiorini@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/CompositingAndBlendingOperator.h>
#include <LibWeb/SVG/AttributeParsing.h>
#include <LibWeb/SVG/SVGAnimatedLength.h>
#include <LibWeb/SVG/SVGElement.h>
#include <LibWeb/SVG/SVGFilterPrimitiveStandardAttributes.h>

namespace Web::SVG {

class SVGFEBlendElement final
    : public SVGElement
    , public SVGFilterPrimitiveStandardAttributes<SVGFEBlendElement> {
    WEB_WRAPPABLE(SVGFEBlendElement, SVGElement);
    GC_DECLARE_ALLOCATOR(SVGFEBlendElement);

public:
    static constexpr u16 SVG_FEBLEND_MODE_UNKNOWN = 0;
    static constexpr u16 SVG_FEBLEND_MODE_NORMAL = 1;
    static constexpr u16 SVG_FEBLEND_MODE_MULTIPLY = 2;
    static constexpr u16 SVG_FEBLEND_MODE_SCREEN = 3;
    static constexpr u16 SVG_FEBLEND_MODE_DARKEN = 4;
    static constexpr u16 SVG_FEBLEND_MODE_LIGHTEN = 5;
    static constexpr u16 SVG_FEBLEND_MODE_OVERLAY = 6;
    static constexpr u16 SVG_FEBLEND_MODE_COLOR_DODGE = 7;
    static constexpr u16 SVG_FEBLEND_MODE_COLOR_BURN = 8;
    static constexpr u16 SVG_FEBLEND_MODE_HARD_LIGHT = 9;
    static constexpr u16 SVG_FEBLEND_MODE_SOFT_LIGHT = 10;
    static constexpr u16 SVG_FEBLEND_MODE_DIFFERENCE = 11;
    static constexpr u16 SVG_FEBLEND_MODE_EXCLUSION = 12;
    static constexpr u16 SVG_FEBLEND_MODE_HUE = 13;
    static constexpr u16 SVG_FEBLEND_MODE_SATURATION = 14;
    static constexpr u16 SVG_FEBLEND_MODE_COLOR = 15;
    static constexpr u16 SVG_FEBLEND_MODE_LUMINOSITY = 16;

    virtual ~SVGFEBlendElement() override = default;

    GC::Ref<SVGAnimatedString> in1();
    GC::Ref<SVGAnimatedString> in2();

    Gfx::CompositingAndBlendingOperator mode() const;
    GC::Ref<SVGAnimatedEnumeration> mode_for_bindings();

private:
    SVGFEBlendElement(DOM::Document&, DOM::QualifiedName);
    virtual void visit_edges(Cell::Visitor&) override;

    virtual void attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& new_value, Optional<Utf16FlyString> const& namespace_) override;

    GC::Ptr<SVGAnimatedString> m_in1;
    GC::Ptr<SVGAnimatedString> m_in2;

    Optional<CSS::MixBlendMode> m_mode;
};

}
