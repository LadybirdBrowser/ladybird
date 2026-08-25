/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <LibTest/TestCase.h>
#include <LibWeb/CSS/NumericType.h>
#include <LibWeb/CSS/RustStyleBridge.h>
#include <LibWeb/CSS/ValueType.h>

namespace {

using Web::CSS::NumericType;
using BaseType = NumericType::BaseType;
using Web::CSS::ValueType;
using Web::CSS::StyleValueFFI::FfiNumericTypeMatch;
using Web::CSS::StyleValueFFI::FfiNumericTypeOperation;

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

bool matches_dimension(NumericType const& type, BaseType base_type, Optional<ValueType> percentages_resolve_as)
{
    switch (base_type) {
    case BaseType::Length:
        return type.matches_length(percentages_resolve_as);
    case BaseType::Angle:
        return type.matches_angle(percentages_resolve_as);
    case BaseType::Time:
        return type.matches_time(percentages_resolve_as);
    case BaseType::Frequency:
        return type.matches_frequency(percentages_resolve_as);
    case BaseType::Resolution:
        return type.matches_resolution(percentages_resolve_as);
    case BaseType::Flex:
        return type.matches_flex(percentages_resolve_as);
    case BaseType::Percent:
    case BaseType::__Count:
        VERIFY_NOT_REACHED();
    }
    VERIFY_NOT_REACHED();
}

bool rust_matches(FfiNumericTypeMatch match_kind, Web::CSS::StyleValueFFI::FfiNumericType const& type, BaseType base_type, Optional<ValueType> percentages_resolve_as)
{
    return Web::CSS::invoke_rust_numeric_type_matches(
        match_kind,
        &type,
        to_underlying(base_type),
        percentages_resolve_as.has_value(),
        percentages_resolve_as.has_value() ? to_underlying(*percentages_resolve_as) : 0);
}

}

TEST_CASE(numeric_type_operations_match)
{
    auto types = enumerate_types();
    for (auto const& first : types) {
        auto ffi_first = to_ffi(first);

        expect_matches(first.inverted(), Web::CSS::invoke_rust_numeric_type_operate(FfiNumericTypeOperation::Invert, &ffi_first, nullptr, 0));

        if (!first.percent_hint().has_value()) {
            for (auto base = 0; base < to_underlying(BaseType::__Count); ++base) {
                auto expected = first;
                expected.apply_percent_hint(static_cast<BaseType>(base));
                expect_matches(expected, Web::CSS::invoke_rust_numeric_type_operate(FfiNumericTypeOperation::ApplyPercentHint, &ffi_first, nullptr, base));
            }
        }

        EXPECT_EQ(first.matches_percentage(), rust_matches(FfiNumericTypeMatch::Percentage, ffi_first, BaseType::Length, {}));
        EXPECT_EQ(first.matches_dimension(), rust_matches(FfiNumericTypeMatch::DimensionCategory, ffi_first, BaseType::Length, {}));

        Array<Optional<ValueType>, 9> const resolve_as_values {
            Optional<ValueType> {},
            ValueType::Number,
            ValueType::Length,
            ValueType::Angle,
            ValueType::Time,
            ValueType::Frequency,
            ValueType::Resolution,
            ValueType::Flex,
            ValueType::Percentage,
        };
        for (auto resolve_as : resolve_as_values) {
            EXPECT_EQ(first.matches_number(resolve_as), rust_matches(FfiNumericTypeMatch::Number, ffi_first, BaseType::Length, resolve_as));
            for (auto base = 0; base < to_underlying(BaseType::Percent); ++base) {
                auto base_type = static_cast<BaseType>(base);
                EXPECT_EQ(matches_dimension(first, base_type, resolve_as), rust_matches(FfiNumericTypeMatch::Dimension, ffi_first, base_type, resolve_as));
                EXPECT_EQ(first.matches_percentage() || matches_dimension(first, base_type, resolve_as), rust_matches(FfiNumericTypeMatch::DimensionPercentage, ffi_first, base_type, resolve_as));
            }
        }

        for (auto const& second : types) {
            // NB: Pairs with a percent hint on either side whose direct-match step fails
            //     reach the provisional hint loop, whose hint application asserts on an
            //     already-hinted type in both implementations; such pairs cannot come out
            //     of the real algebra (hints arise from the unhinted additions covered
            //     below), so hinted operands are only exercised as identical pairs.
            if ((first.percent_hint().has_value() || second.percent_hint().has_value()) && first != second)
                continue;
            auto ffi_second = to_ffi(second);
            expect_matches(first.added_to(second), Web::CSS::invoke_rust_numeric_type_operate(FfiNumericTypeOperation::Add, &ffi_first, &ffi_second, 0));
            expect_matches(first.multiplied_by(second), Web::CSS::invoke_rust_numeric_type_operate(FfiNumericTypeOperation::Multiply, &ffi_first, &ffi_second, 0));
            expect_matches(first.made_consistent_with(second), Web::CSS::invoke_rust_numeric_type_operate(FfiNumericTypeOperation::MakeConsistent, &ffi_first, &ffi_second, 0));
        }
    }
}
