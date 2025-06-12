/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/ProxyConstructor.h>
#include <LibJS/Runtime/ProxyObject.h>

namespace JS {

GC_DEFINE_ALLOCATOR(ProxyConstructor);

// 10.5.14 ProxyCreate ( target, handler ), https://tc39.es/ecma262/#sec-proxycreate
static ThrowCompletionOr<ProxyObject*> proxy_create(VM& vm, Value target, Value handler)
{
    auto& realm = *vm.current_realm();

    // 1. If target is not an Object, throw a TypeError exception.
    if (!target.is_object())
        return vm.throw_completion<TypeError>(ErrorType::ProxyConstructorBadType, "target", target.to_string_without_side_effects());

    // 2. If handler is not an Object, throw a TypeError exception.
    if (!handler.is_object())
        return vm.throw_completion<TypeError>(ErrorType::ProxyConstructorBadType, "handler", handler.to_string_without_side_effects());

    // 3. Let P be MakeBasicObject(« [[ProxyHandler]], [[ProxyTarget]] »).
    // 4. Set P's essential internal methods, except for [[Call]] and [[Construct]], to the definitions specified in 10.5.
    // 5.  IsCallable(target) is true, then
    //    a. Set P.[[Call]] as specified in 10.5.12.
    //    b. If IsConstructor(target) is true, then
    //        i. Set P.[[Construct]] as specified in 10.5.13.
    // 6. Set P.[[ProxyTarget]] to target.
    // 7. Set P.[[ProxyHandler]] to handler.
    // 8. Return P.
    return ProxyObject::create(realm, target.as_object(), handler.as_object()).ptr();
}

ProxyConstructor::ProxyConstructor(Realm& realm)
    : NativeFunction(realm.vm().names.Proxy.as_string(), realm.intrinsics().function_prototype())
{
}

void ProxyConstructor::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);
    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.revocable, revocable, 2, attr);

    define_direct_property(vm.names.length, Value(2), Attribute::Configurable);
}

// 28.2.1.1 Proxy ( target, handler ), https://tc39.es/ecma262/#sec-proxy-target-handler
ThrowCompletionOr<Value> ProxyConstructor::call()
{
    auto& vm = this->vm();

    // 1. If NewTarget is undefined, throw a TypeError exception.
    return vm.throw_completion<TypeError>(ErrorType::ConstructorWithoutNew, vm.names.Proxy);
}

// 28.2.1.1 Proxy ( target, handler ), https://tc39.es/ecma262/#sec-proxy-target-handler
ThrowCompletionOr<GC::Ref<Object>> ProxyConstructor::construct(FunctionObject&)
{
    auto& vm = this->vm();
    auto target = vm.argument(0);
    auto handler = vm.argument(1);

    // 2. Return ? ProxyCreate(target, handler).
    return *TRY(proxy_create(vm, target, handler));
}

// 28.2.2.1 Proxy.revocable ( target, handler ), https://tc39.es/ecma262/#sec-proxy.revocable
JS_DEFINE_NATIVE_FUNCTION(ProxyConstructor::revocable)
{
    auto& realm = *vm.current_realm();
    auto target = vm.argument(0);
    auto handler = vm.argument(1);

    // 1. Let p be ? ProxyCreate(target, handler).
    auto* proxy = TRY(proxy_create(vm, target, handler));

    // 2. Let revokerClosure be a new Abstract Closure with no parameters that captures nothing and performs the following steps when called:
    auto revoker_closure = [proxy_handle = make_root(proxy)](auto&) -> ThrowCompletionOr<Value> {
        // a. Let F be the active function object.

        // b. Let p be F.[[RevocableProxy]].
        auto& proxy = const_cast<ProxyObject&>(*proxy_handle.cell());

        // c. If p is null, return undefined.
        if (proxy.is_revoked())
            return js_undefined();

        // d. Set F.[[RevocableProxy]] to null.
        // e. Assert: p is a Proxy object.
        // f. Set p.[[ProxyTarget]] to null.
        // g. Set p.[[ProxyHandler]] to null.
        proxy.revoke();

        // h. Return undefined.
        return js_undefined();
    };

    // 3. Let revoker be CreateBuiltinFunction(revokerClosure, 0, "", « [[RevocableProxy]] »).
    // 4. Set revoker.[[RevocableProxy]] to p.
    auto revoker = NativeFunction::create(realm, move(revoker_closure), 0);

    // 5. Let result be OrdinaryObjectCreate(%Object.prototype%).
    auto result = Object::create(realm, realm.intrinsics().object_prototype());

    // 6. Perform ! CreateDataPropertyOrThrow(result, "proxy", p).
    MUST(result->create_data_property_or_throw(vm.names.proxy, proxy));

    // 7. Perform ! CreateDataPropertyOrThrow(result, "revoke", revoker).
    MUST(result->create_data_property_or_throw(vm.names.revoke, revoker));

    // 8. Return result.
    return result;
}

}
