/*
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <LibGC/Ptr.h>
#include <LibJS/Export.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/EngineCell.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Value.h>

namespace JS {

// 27.2.1.1 PromiseCapability Records, https://tc39.es/ecma262/#sec-promisecapability-records
class JS_API PromiseCapability final : public EngineCell {
public:
    [[nodiscard]] GC::Ref<Object> promise() const;

    [[nodiscard]] GC::Ref<FunctionObject> resolve() const;

    [[nodiscard]] GC::Ref<FunctionObject> reject() const;
};

// 27.2.1.1.1 IfAbruptRejectPromise ( value, capability ), https://tc39.es/ecma262/#sec-ifabruptrejectpromise
#define __TRY_OR_REJECT(vm, capability, expression, CALL_CHECK)                                                                             \
    ({                                                                                                                                      \
        auto&& _temporary_try_or_reject_result = (expression);                                                                              \
        /* 1. If value is an abrupt completion, then */                                                                                     \
        if (_temporary_try_or_reject_result.is_error()) {                                                                                   \
            /* a. Perform ? Call(capability.[[Reject]], undefined, « value.[[Value]] »). */                                                 \
            CALL_CHECK(JS::call(vm, *(capability)->reject(), JS::js_undefined(), _temporary_try_or_reject_result.release_error().value())); \
                                                                                                                                            \
            /* b. Return capability.[[Promise]]. */                                                                                         \
            return (capability)->promise();                                                                                                 \
        }                                                                                                                                   \
                                                                                                                                            \
        static_assert(!::AK::Detail::IsLvalueReference<decltype(_temporary_try_or_reject_result.release_value())>,                          \
            "Do not return a reference from a fallible expression");                                                                        \
                                                                                                                                            \
        /* 2. Else if value is a Completion Record, set value to value.[[Value]]. */                                                        \
        _temporary_try_or_reject_result.release_value();                                                                                    \
    })

#define TRY_OR_REJECT(vm, capability, expression) \
    __TRY_OR_REJECT(vm, capability, expression, TRY)

#define TRY_OR_MUST_REJECT(vm, capability, expression) \
    __TRY_OR_REJECT(vm, capability, expression, MUST)

// 27.2.1.5 NewPromiseCapability ( C ), https://tc39.es/ecma262/#sec-newpromisecapability
JS_API ThrowCompletionOr<GC::Ref<PromiseCapability>> new_promise_capability(VM& vm, Value constructor);

}
