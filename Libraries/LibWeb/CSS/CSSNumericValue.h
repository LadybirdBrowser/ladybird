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
    WebIDL::Long length {};
    WebIDL::Long angle {};
    WebIDL::Long time {};
    WebIDL::Long frequency {};
    WebIDL::Long resolution {};
    WebIDL::Long flex {};
    WebIDL::Long percent {};
    Optional<Bindings::CSSNumericBaseType> percent_hint {};
};

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

    CSSNumericType type_for_bindings() const;
    NumericType const& type() const { return m_type; }

    virtual String to_string() const final override { return to_string({}); }
    String to_string(SerializationParams const&) const;

protected:
    explicit CSSNumericValue(JS::Realm&, NumericType);

    virtual void initialize(JS::Realm&) override;

    NumericType m_type;
};

// https://drafts.css-houdini.org/css-typed-om-1/#typedefdef-cssnumberish
using CSSNumberish = Variant<double, GC::Root<CSSNumericValue>>;

GC::Ref<CSSNumericValue> rectify_a_numberish_value(JS::Realm&, CSSNumberish const&, Optional<FlyString> unit = {});

}
