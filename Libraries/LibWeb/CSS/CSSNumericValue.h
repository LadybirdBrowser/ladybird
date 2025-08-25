/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <LibWeb/Bindings/CSSNumericValuePrototype.h>
#include <LibWeb/CSS/CSSStyleValue.h>
#include <LibWeb/CSS/NumericType.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::CSS {

struct CSSNumericType {
    Optional<WebIDL::Long> length;
    Optional<WebIDL::Long> angle;
    Optional<WebIDL::Long> time;
    Optional<WebIDL::Long> frequency;
    Optional<WebIDL::Long> resolution;
    Optional<WebIDL::Long> flex;
    Optional<WebIDL::Long> percent;
    Optional<Bindings::CSSNumericBaseType> percent_hint;
};

// https://drafts.css-houdini.org/css-typed-om-1/#typedefdef-cssnumberish
using CSSNumberish = Variant<double, GC::Root<CSSNumericValue>>;

// https://drafts.css-houdini.org/css-typed-om-1/#cssnumericvalue
class CSSNumericValue : public CSSStyleValue {
    WEB_PLATFORM_OBJECT(CSSNumericValue, CSSStyleValue);
    GC_DECLARE_ALLOCATOR(CSSNumericValue);

public:
    struct SerializationParams {
        Optional<double> minimum {};
        Optional<double> maximum {};
        bool nested { false };
        bool parenless { false };
    };
    virtual ~CSSNumericValue() override = default;

    bool equals_for_bindings(Vector<CSSNumberish>) const;
    virtual bool is_equal_numeric_value(GC::Ref<CSSNumericValue> other) const = 0;

    CSSNumericType type_for_bindings() const;
    NumericType const& type() const { return m_type; }

    virtual String to_string() const final override { return to_string({}); }
    String to_string(SerializationParams const&) const;

    static WebIDL::ExceptionOr<GC::Ref<CSSNumericValue>> parse(JS::VM&, String const& css_text);

protected:
    explicit CSSNumericValue(JS::Realm&, NumericType);

    virtual void initialize(JS::Realm&) override;

    NumericType m_type;
};

GC::Ref<CSSNumericValue> rectify_a_numberish_value(JS::Realm&, CSSNumberish const&, Optional<FlyString> unit = {});

}
