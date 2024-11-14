/*
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/AbstractOperations.h>

namespace JS {

// 27.2.1.1 PromiseCapability Records, https://tc39.es/ecma262/#sec-promisecapability-records
class PromiseCapability final : public Cell {
    GC_CELL(PromiseCapability, Cell);
    GC_DECLARE_ALLOCATOR(PromiseCapability);

public:
    static GC::Ref<PromiseCapability> create(VM& vm, GC::Ref<Object> promise, GC::Ref<FunctionObject> resolve, GC::Ref<FunctionObject> reject);

    virtual ~PromiseCapability() = default;

    [[nodiscard]] GC::Ref<Object> promise() const { return m_promise; }

    [[nodiscard]] GC::Ref<FunctionObject> resolve() const { return m_resolve; }

    [[nodiscard]] GC::Ref<FunctionObject> reject() const { return m_reject; }

private:
    PromiseCapability(GC::Ref<Object>, GC::Ref<FunctionObject>, GC::Ref<FunctionObject>);

    virtual void visit_edges(Visitor&) override;

    GC::Ref<Object> m_promise;
    GC::Ref<FunctionObject> m_resolve;
    GC::Ref<FunctionObject> m_reject;
};

// 27.2.1.1.1 IfAbruptRejectPromise ( value, capability ), https://tc39.es/ecma262/#sec-ifabruptrejectpromise
#define __TRY_OR_REJECT(vm, capability, expression, CALL_CHECK)                                                                          \
    ({                                                                                                                                   \
        auto&& _temporary_try_or_reject_result = (expression);                                                                           \
        /* 1. If value is an abrupt completion, then */                                                                                  \
        if (_temporary_try_or_reject_result.is_error()) {                                                                                \
            /* a. Perform ? Call(capability.[[Reject]], undefined, « value.[[Value]] »). */                                            \
            CALL_CHECK(JS::call(vm, *(capability)->reject(), js_undefined(), *_temporary_try_or_reject_result.release_error().value())); \
                                                                                                                                         \
            /* b. Return capability.[[Promise]]. */                                                                                      \
            return (capability)->promise();                                                                                              \
        }                                                                                                                                \
                                                                                                                                         \
        static_assert(!::AK::Detail::IsLvalueReference<decltype(_temporary_try_or_reject_result.release_value())>,                       \
            "Do not return a reference from a fallible expression");                                                                     \
                                                                                                                                         \
        /* 2. Else if value is a Completion Record, set value to value.[[Value]]. */                                                     \
        _temporary_try_or_reject_result.release_value();                                                                                 \
    })

#define TRY_OR_REJECT(vm, capability, expression) \
    __TRY_OR_REJECT(vm, capability, expression, TRY)

#define TRY_OR_MUST_REJECT(vm, capability, expression) \
    __TRY_OR_REJECT(vm, capability, expression, MUST)

// 27.2.1.1.1 IfAbruptRejectPromise ( value, capability ), https://tc39.es/ecma262/#sec-ifabruptrejectpromise
#define TRY_OR_REJECT_WITH_VALUE(vm, capability, expression)                                                                      \
    ({                                                                                                                            \
        auto&& _temporary_try_or_reject_result = (expression);                                                                    \
        /* 1. If value is an abrupt completion, then */                                                                           \
        if (_temporary_try_or_reject_result.is_error()) {                                                                         \
            /* a. Perform ? Call(capability.[[Reject]], undefined, « value.[[Value]] »). */                                     \
            TRY(JS::call(vm, *(capability)->reject(), js_undefined(), *_temporary_try_or_reject_result.release_error().value())); \
                                                                                                                                  \
            /* b. Return capability.[[Promise]]. */                                                                               \
            return Value { (capability)->promise() };                                                                             \
        }                                                                                                                         \
                                                                                                                                  \
        static_assert(!::AK::Detail::IsLvalueReference<decltype(_temporary_try_or_reject_result.release_value())>,                \
            "Do not return a reference from a fallible expression");                                                              \
                                                                                                                                  \
        /* 2. Else if value is a Completion Record, set value to value.[[Value]]. */                                              \
        _temporary_try_or_reject_result.release_value();                                                                          \
    })

// 27.2.1.5 NewPromiseCapability ( C ), https://tc39.es/ecma262/#sec-newpromisecapability
ThrowCompletionOr<GC::Ref<PromiseCapability>> new_promise_capability(VM& vm, Value constructor);

}
