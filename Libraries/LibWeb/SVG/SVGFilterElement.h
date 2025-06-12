/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/AttributeParser.h>
#include <LibWeb/SVG/SVGAnimatedEnumeration.h>
#include <LibWeb/SVG/SVGAnimatedLength.h>
#include <LibWeb/SVG/SVGElement.h>
#include <LibWeb/SVG/SVGURIReference.h>

namespace Web::SVG {

// https://drafts.fxtf.org/filter-effects/#elementdef-filter
class SVGFilterElement final
    : public SVGElement
    , public SVGURIReferenceMixin<SupportsXLinkHref::No> {
    WEB_PLATFORM_OBJECT(SVGFilterElement, SVGElement);
    GC_DECLARE_ALLOCATOR(SVGFilterElement);

public:
    virtual ~SVGFilterElement() override = default;

    // ^DOM::Element
    virtual void apply_presentational_hints(GC::Ref<CSS::CascadedProperties>) const override;
    virtual bool is_presentational_hint(AK::FlyString const&) const override;

    virtual void attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_) override;

    GC::Ref<SVGAnimatedEnumeration> filter_units() const;
    GC::Ref<SVGAnimatedEnumeration> primitive_units() const;
    GC::Ref<SVGAnimatedLength> x() const;
    GC::Ref<SVGAnimatedLength> y() const;
    GC::Ref<SVGAnimatedLength> width() const;
    GC::Ref<SVGAnimatedLength> height() const;

private:
    SVGFilterElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;

    Optional<SVGUnits> m_filter_units {};
    Optional<SVGUnits> m_primitive_units {};
};

}
