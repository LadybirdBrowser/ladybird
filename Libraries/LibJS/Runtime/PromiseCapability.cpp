/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/EmbeddingABIConversions.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/PromiseCapability.h>
#include <LibJS/Runtime/VM.h>

namespace JS {

using namespace EmbeddingABI;

static JSPromiseCapability* promise_capability_to_abi(PromiseCapability const& promise_capability)
{
    return cell_to_abi<JSPromiseCapability>(promise_capability);
}

static FunctionObject& function_from_abi(JSObject* function)
{
    return static_cast<FunctionObject&>(object_from_abi(function));
}

GC::Ref<Object> PromiseCapability::promise() const
{
    return object_from_abi(js_promise_capability_promise(promise_capability_to_abi(*this)));
}

GC::Ref<FunctionObject> PromiseCapability::resolve() const
{
    return function_from_abi(js_promise_capability_resolve(promise_capability_to_abi(*this)));
}

GC::Ref<FunctionObject> PromiseCapability::reject() const
{
    return function_from_abi(js_promise_capability_reject(promise_capability_to_abi(*this)));
}

// 27.2.1.5 NewPromiseCapability ( C ), https://tc39.es/ecma262/#sec-newpromisecapability
ThrowCompletionOr<GC::Ref<PromiseCapability>> new_promise_capability(VM& vm, Value constructor)
{
    return completion_from_abi<GC::Ref<PromiseCapability>>(js_promise_capability_new(vm_to_abi(vm), value_to_abi(constructor)));
}

}
