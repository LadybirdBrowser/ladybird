/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <LibWeb/CSS/CSSNumericValue.h>

namespace Web::CSS {

// https://drafts.css-houdini.org/css-typed-om-1/#cssunitvalue
class CSSUnitValue final : public CSSNumericValue {
    WEB_PLATFORM_OBJECT(CSSUnitValue, CSSNumericValue);
    GC_DECLARE_ALLOCATOR(CSSUnitValue);

public:
    [[nodiscard]] static GC::Ref<CSSUnitValue> create(JS::Realm&, double value, FlyString unit);
    static GC::Ptr<CSSUnitValue> create_from_sum_value_item(JS::Realm&, SumValueItem const&);
    static WebIDL::ExceptionOr<GC::Ref<CSSUnitValue>> construct_impl(JS::Realm&, double value, FlyString unit);

    virtual ~CSSUnitValue() override = default;

    double value() const { return m_value; }
    void set_value(double value);

    FlyString const& unit() const { return m_unit; }

    String serialize_unit_value(Optional<double> minimum, Optional<double> maximum) const;

    GC::Ptr<CSSUnitValue> converted_to_unit(FlyString const& unit) const;

    virtual bool is_equal_numeric_value(GC::Ref<CSSNumericValue> other) const override;
    virtual Optional<SumValue> create_a_sum_value() const override;

private:
    explicit CSSUnitValue(JS::Realm&, double value, FlyString unit, NumericType type);

    virtual void initialize(JS::Realm&) override;

    double m_value;
    FlyString m_unit;
};

}
