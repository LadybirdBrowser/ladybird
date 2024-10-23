/*
 * Copyright (c) 2020-2024, the SerenityOS developers
 * Copyright (c) 2024, the Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Heap/MarkedVector.h>
#include <LibJS/Runtime/PropertyKey.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/Runtime/Value.h>

namespace JS {

template<typename... Args>
[[nodiscard]] ALWAYS_INLINE ThrowCompletionOr<Value> Value::invoke(VM& vm, PropertyKey const& property_key, Args... args)
{
    if constexpr (sizeof...(Args) > 0) {
        MarkedVector<Value> arglist { vm.heap() };
        (..., arglist.append(move(args)));
        return invoke_internal(vm, property_key, move(arglist));
    }

    return invoke_internal(vm, property_key, Optional<MarkedVector<Value>> {});
}

}
