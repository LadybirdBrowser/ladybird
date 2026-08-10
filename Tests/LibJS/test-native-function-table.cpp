/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/VM.h>
#include <LibTest/TestCase.h>

static JS::ThrowCompletionOr<JS::Value> first_native_function(JS::VM&)
{
    return JS::js_undefined();
}

static JS::ThrowCompletionOr<JS::Value> second_native_function(JS::VM&)
{
    return JS::js_undefined();
}

TEST_CASE(native_function_registration_deduplicates_entries)
{
    auto vm = JS::VM::create();

    auto first_index = vm->register_native_function(first_native_function, JS::NativeFunctionType::RawNativeFunction);
    auto duplicate_index = vm->register_native_function(first_native_function, JS::NativeFunctionType::RawNativeFunction);
    auto second_index = vm->register_native_function(second_native_function, JS::NativeFunctionType::RawNativeFunction);

    EXPECT_EQ(duplicate_index, first_index);
    EXPECT_NE(second_index, first_index);
    EXPECT_EQ(vm->native_function(first_index, JS::NativeFunctionType::RawNativeFunction), first_native_function);
    EXPECT_EQ(vm->native_function(second_index, JS::NativeFunctionType::RawNativeFunction), second_native_function);
}
