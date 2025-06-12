/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Value.h>

namespace JS {

inline bool Value::to_boolean() const
{
    // OPTIMIZATION: Fast path for when this value is already a boolean.
    if (is_boolean())
        return as_bool();

    if (is_int32())
        return as_i32() != 0;

    return to_boolean_slow_case();
}

inline ThrowCompletionOr<Value> Value::to_number(VM& vm) const
{
    // OPTIMIZATION: Fast path for when this value is already a number.
    if (is_number())
        return *this;

    return to_number_slow_case(vm);
}

inline ThrowCompletionOr<Value> Value::to_numeric(VM& vm) const
{
    // OPTIMIZATION: Fast path for when this value is already a number.
    if (is_number())
        return *this;

    return to_numeric_slow_case(vm);
}

inline ThrowCompletionOr<Value> Value::to_primitive(VM& vm, PreferredType preferred_type) const
{
    if (!is_object())
        return *this;
    return to_primitive_slow_case(vm, preferred_type);
}

}
