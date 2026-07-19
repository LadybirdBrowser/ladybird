/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWeb/CSS/NumericType.h>
#include <LibWeb/CSS/RustStyleBridge.h>

namespace {

using Web::CSS::NumericType;
using BaseType = NumericType::BaseType;

Web::CSS::StyleValueFFI::FfiNumericType to_ffi(NumericType const& type)
{
    Web::CSS::StyleValueFFI::FfiNumericType result {};
    for (auto i = 0; i < to_underlying(BaseType::__Count); ++i) {
        if (auto exponent = type.exponent(static_cast<BaseType>(i)); exponent.has_value()) {
            result.has_exponent[i] = true;
            result.exponents[i] = exponent.value();
        }
    }
    if (auto hint = type.percent_hint(); hint.has_value()) {
        result.has_percent_hint = true;
        result.percent_hint = to_underlying(hint.value());
    }
    result.valid = true;
    return result;
}

void expect_matches(Optional<NumericType> const& expected, Web::CSS::StyleValueFFI::FfiNumericType const& actual)
{
    EXPECT_EQ(expected.has_value(), actual.valid);
    if (!expected.has_value() || !actual.valid)
        return;
    for (auto i = 0; i < to_underlying(BaseType::__Count); ++i) {
        auto exponent = expected->exponent(static_cast<BaseType>(i));
        EXPECT_EQ(exponent.has_value(), actual.has_exponent[i]);
        if (exponent.has_value())
            EXPECT_EQ(exponent.value(), actual.exponents[i]);
    }
    auto hint = expected->percent_hint();
    EXPECT_EQ(hint.has_value(), actual.has_percent_hint);
    if (hint.has_value())
        EXPECT_EQ(to_underlying(hint.value()), actual.percent_hint);
}

Vector<NumericType> enumerate_types()
{
    Vector<NumericType> types;
    types.append(NumericType {});
    for (auto base = 0; base < to_underlying(BaseType::__Count); ++base) {
        for (auto exponent : { -1, 0, 1, 2 }) {
            NumericType type { static_cast<BaseType>(base), exponent };
            types.append(type);

            // A hinted variant, when the hint is applicable.
            if (auto hinted = type; !hinted.percent_hint().has_value()) {
                hinted.apply_percent_hint(static_cast<BaseType>(base));
                types.append(hinted);
            }
        }
    }
    // A couple of compound types.
    NumericType length_per_time { BaseType::Length, 1 };
    length_per_time.set_exponent(BaseType::Time, -1);
    types.append(length_per_time);
    NumericType length_percent { BaseType::Length, 1 };
    length_percent.set_exponent(BaseType::Percent, 1);
    types.append(length_percent);
    return types;
}

}

TEST_CASE(numeric_type_operations_match)
{
    auto types = enumerate_types();
    for (auto const& first : types) {
        auto ffi_first = to_ffi(first);

        expect_matches(first.inverted(), Web::CSS::invoke_rust_numeric_type_operate(2, &ffi_first, &ffi_first));

        for (auto const& second : types) {
            // NB: Pairs with a percent hint on either side whose direct-match step fails
            //     reach the provisional hint loop, whose hint application asserts on an
            //     already-hinted type in both implementations; such pairs cannot come out
            //     of the real algebra (hints arise from the unhinted additions covered
            //     below), so hinted operands are only exercised as identical pairs.
            if ((first.percent_hint().has_value() || second.percent_hint().has_value()) && first != second)
                continue;
            auto ffi_second = to_ffi(second);
            expect_matches(first.added_to(second), Web::CSS::invoke_rust_numeric_type_operate(0, &ffi_first, &ffi_second));
            expect_matches(first.multiplied_by(second), Web::CSS::invoke_rust_numeric_type_operate(1, &ffi_first, &ffi_second));
            expect_matches(first.made_consistent_with(second), Web::CSS::invoke_rust_numeric_type_operate(3, &ffi_first, &ffi_second));
        }
    }
}
