/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Optional.h>
#include <LibWeb/SVG/SVGAnimatedEnumeration.h>
#include <LibWeb/SVG/SVGAnimatedNumber.h>
#include <LibWeb/SVG/SVGAnimatedNumberList.h>
#include <LibWeb/SVG/SVGElement.h>

namespace Web::SVG {

// https://www.w3.org/TR/filter-effects-1/#InterfaceSVGComponentTransferFunctionElement
class SVGComponentTransferFunctionElement
    : public SVGElement {
    WEB_PLATFORM_OBJECT(SVGComponentTransferFunctionElement, SVGElement);

public:
    enum class Type : u8 {
        Unknown = 0,
        Identity = 1,
        Table = 2,
        Discrete = 3,
        Linear = 4,
        Gamma = 5,
    };

    virtual ~SVGComponentTransferFunctionElement() override = default;

    virtual void attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_) override;

    GC::Ref<SVGAnimatedEnumeration> type();
    GC::Ref<SVGAnimatedNumberList> table_values();
    GC::Ref<SVGAnimatedNumber> slope();
    GC::Ref<SVGAnimatedNumber> intercept();
    GC::Ref<SVGAnimatedNumber> amplitude();
    GC::Ref<SVGAnimatedNumber> exponent();
    GC::Ref<SVGAnimatedNumber> offset();

    Vector<float> table_float_values();
    ReadonlyBytes color_table();

protected:
    SVGComponentTransferFunctionElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;

private:
    virtual void visit_edges(Cell::Visitor&) override;

    Type type_from_attribute() const;

    GC::Ptr<SVGAnimatedEnumeration> m_type;
    GC::Ptr<SVGAnimatedNumberList> m_table_values;
    GC::Ptr<SVGAnimatedNumber> m_slope;
    GC::Ptr<SVGAnimatedNumber> m_intercept;
    GC::Ptr<SVGAnimatedNumber> m_amplitude;
    GC::Ptr<SVGAnimatedNumber> m_exponent;
    GC::Ptr<SVGAnimatedNumber> m_offset;

    Optional<ByteBuffer> m_cached_color_table;
};

}
