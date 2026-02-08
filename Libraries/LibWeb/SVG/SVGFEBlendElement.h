/*
 * Copyright (c) 2025, Lucien Fiorini <lucienfiorini@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/CompositingAndBlendingOperator.h>
#include <LibWeb/SVG/AttributeParser.h>
#include <LibWeb/SVG/SVGAnimatedLength.h>
#include <LibWeb/SVG/SVGElement.h>
#include <LibWeb/SVG/SVGFilterPrimitiveStandardAttributes.h>

namespace Web::SVG {

class SVGFEBlendElement final
    : public SVGElement
    , public SVGFilterPrimitiveStandardAttributes<SVGFEBlendElement> {
    WEB_PLATFORM_OBJECT(SVGFEBlendElement, SVGElement);
    GC_DECLARE_ALLOCATOR(SVGFEBlendElement);

public:
    virtual ~SVGFEBlendElement() override = default;

    GC::Ref<SVGAnimatedString> in1();
    GC::Ref<SVGAnimatedString> in2();

    Gfx::CompositingAndBlendingOperator mode() const;
    GC::Ref<SVGAnimatedEnumeration> mode_for_bindings() const;

private:
    SVGFEBlendElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    virtual void attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& new_value, Optional<FlyString> const& namespace_) override;

    GC::Ptr<SVGAnimatedString> m_in1;
    GC::Ptr<SVGAnimatedString> m_in2;

    Optional<CSS::MixBlendMode> m_mode;
};

}
