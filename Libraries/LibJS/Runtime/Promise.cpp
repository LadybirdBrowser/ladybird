/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/EmbeddingABIConversions.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/Promise.h>
#include <LibJS/Runtime/PromiseCapability.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>

namespace JS {

using namespace EmbeddingABI;

static_assert(to_underlying(Promise::State::Pending) == JS_PROMISE_STATE_PENDING);
static_assert(to_underlying(Promise::State::Fulfilled) == JS_PROMISE_STATE_FULFILLED);
static_assert(to_underlying(Promise::State::Rejected) == JS_PROMISE_STATE_REJECTED);

static FunctionObject& function_from_abi(JSObject* function)
{
    return static_cast<FunctionObject&>(object_from_abi(function));
}

Promise::State Promise::state() const
{
    return static_cast<State>(js_promise_state(object_to_abi(*this)));
}

Value Promise::result() const
{
    return value_from_abi(js_promise_result(object_to_abi(*this)));
}

Promise::ResolvingFunctions Promise::create_resolving_functions()
{
    auto resolving_functions = js_promise_create_resolving_functions(vm_to_abi(vm()), object_to_abi(*this));
    return { function_from_abi(resolving_functions.resolve), function_from_abi(resolving_functions.reject) };
}

Value Promise::perform_then(Value on_fulfilled, Value on_rejected, GC::Ptr<PromiseCapability> result_capability)
{
    auto* abi_result_capability = result_capability ? cell_to_abi<JSPromiseCapability>(*result_capability) : nullptr;
    return value_from_abi(js_promise_perform_then(vm_to_abi(vm()), object_to_abi(*this), value_to_abi(on_fulfilled), value_to_abi(on_rejected), abi_result_capability));
}

bool Promise::is_handled() const
{
    return js_promise_is_handled(object_to_abi(*this));
}

void Promise::set_is_handled()
{
    js_promise_set_is_handled(object_to_abi(*this));
}

}
