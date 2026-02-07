/*
 * Copyright (c) 2026, Undefine <undefine@undefine.pl
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/WebIDL/AbstractOperations.h>

TEST_CASE(convert_to_int)
{
    auto vm = JS::VM::create();
    auto execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);

    auto maybe_i8_value = Web::WebIDL::convert_to_int<i8>(vm, JS::Value(0.0));
    EXPECT(!maybe_i8_value.is_error());
    EXPECT_EQ(maybe_i8_value.value(), 0);

    maybe_i8_value = Web::WebIDL::convert_to_int<i8>(vm, JS::Value(123.0));
    EXPECT(!maybe_i8_value.is_error());
    EXPECT_EQ(maybe_i8_value.value(), 123);

    maybe_i8_value = Web::WebIDL::convert_to_int<i8>(vm, JS::Value(-123.0));
    EXPECT(!maybe_i8_value.is_error());
    EXPECT_EQ(maybe_i8_value.value(), -123);

    auto maybe_u8_value = Web::WebIDL::convert_to_int<u8>(vm, JS::Value(0.0));
    EXPECT(!maybe_u8_value.is_error());
    EXPECT_EQ(maybe_u8_value.value(), 0);

    maybe_u8_value = Web::WebIDL::convert_to_int<u8>(vm, JS::Value(255.0));
    EXPECT(!maybe_u8_value.is_error());
    EXPECT_EQ(maybe_u8_value.value(), 255);

    auto maybe_i32_value = Web::WebIDL::convert_to_int<i32>(vm, JS::Value(12345678.0));
    EXPECT(!maybe_i32_value.is_error());
    EXPECT_EQ(maybe_i32_value.value(), 12345678);

    maybe_i32_value = Web::WebIDL::convert_to_int<i32>(vm, JS::Value(JS::MAX_ARRAY_LIKE_INDEX), Web::WebIDL::EnforceRange::Yes);
    EXPECT(maybe_i32_value.is_error());

    maybe_i32_value = Web::WebIDL::convert_to_int<i32>(vm, JS::Value(JS::MAX_ARRAY_LIKE_INDEX), Web::WebIDL::EnforceRange::No, Web::WebIDL::Clamp::Yes);
    EXPECT(!maybe_i32_value.is_error());
    EXPECT_EQ(maybe_i32_value.value(), AK::NumericLimits<i32>::max());

    auto maybe_i64_value = Web::WebIDL::convert_to_int<i64>(vm, JS::Value(987654321.0));
    EXPECT(!maybe_i64_value.is_error());
    EXPECT_EQ(maybe_i64_value.value(), 987654321);

    maybe_i64_value = Web::WebIDL::convert_to_int<i64>(vm, JS::Value(-1.0));
    EXPECT(!maybe_i64_value.is_error());
    EXPECT_EQ(maybe_i64_value.value(), -1);

    maybe_i64_value = Web::WebIDL::convert_to_int<i64>(vm, JS::Value(0));
    EXPECT(!maybe_i64_value.is_error());
    EXPECT_EQ(maybe_i64_value.value(), 0);

    maybe_i64_value = Web::WebIDL::convert_to_int<i64>(vm, JS::js_nan());
    EXPECT(!maybe_i64_value.is_error());
    EXPECT_EQ(maybe_i64_value.value(), 0);

    maybe_i64_value = Web::WebIDL::convert_to_int<i64>(vm, JS::js_infinity());
    EXPECT(!maybe_i64_value.is_error());
    EXPECT_EQ(maybe_i64_value.value(), 0);

    maybe_i64_value = Web::WebIDL::convert_to_int<i64>(vm, JS::Value(JS::MAX_ARRAY_LIKE_INDEX));
    EXPECT(!maybe_i64_value.is_error());
    EXPECT_EQ(maybe_i64_value.value(), JS::MAX_ARRAY_LIKE_INDEX);

    maybe_i64_value = Web::WebIDL::convert_to_int<i64>(vm, JS::js_nan(), Web::WebIDL::EnforceRange::Yes);
    EXPECT(maybe_i64_value.is_error());
}
