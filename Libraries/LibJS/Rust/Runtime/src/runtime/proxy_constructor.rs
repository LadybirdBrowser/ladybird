/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use ak::Utf16FlyString;
use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::completion::{Must, ThrowCompletionOr};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::function_object::FunctionObject;
use crate::runtime::native_function::{NativeFunction, define_native_function_class, raw_native};
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::proxy_object::ProxyObject;
use crate::runtime::realm::Realm;

// 10.5.14 ProxyCreate ( target, handler ), https://tc39.es/ecma262/#sec-proxycreate
fn proxy_create(vm: &Vm, target: Value, handler: Value) -> ThrowCompletionOr<Gc<ProxyObject>> {
    let realm = vm.current_realm().expect("there is a current realm");

    // 1. If target is not an Object, throw a TypeError exception.
    if !target.is_object() {
        return vm.throw_completion(
            ErrorKind::TypeError,
            ErrorType::ProxyConstructorBadType,
            &[&"target", &target],
        );
    }

    // 2. If handler is not an Object, throw a TypeError exception.
    if !handler.is_object() {
        return vm.throw_completion(
            ErrorKind::TypeError,
            ErrorType::ProxyConstructorBadType,
            &[&"handler", &handler],
        );
    }

    // 3. Let P be MakeBasicObject(« [[ProxyHandler]], [[ProxyTarget]] »).
    // 4. Set P's essential internal methods, except for [[Call]] and [[Construct]], to the definitions specified in 10.5.
    // 5.  IsCallable(target) is true, then
    //    a. Set P.[[Call]] as specified in 10.5.12.
    //    b. If IsConstructor(target) is true, then
    //        i. Set P.[[Construct]] as specified in 10.5.13.
    // 6. Set P.[[ProxyTarget]] to target.
    // 7. Set P.[[ProxyHandler]] to handler.
    // 8. Return P.
    Ok(ProxyObject::create(vm, realm, target.as_object(), handler.as_object()))
}

#[repr(C)]
#[derive(Trace)]
pub struct ProxyConstructor {
    base: NativeFunction,
}

define_native_function_class!(
    ProxyConstructor,
    initialize: ProxyConstructor::initialize,
    call: ProxyConstructor::call,
    construct: ProxyConstructor::construct
);

impl ProxyConstructor {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<ProxyConstructor> {
        realm.create_object(
            vm,
            ProxyConstructor {
                base: NativeFunction::new_with_name(
                    vm,
                    Self::CLASS,
                    vm.names.Proxy.as_string().clone(),
                    realm.function_prototype(),
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        let attributes = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        object.define_native_function(
            vm,
            realm,
            &vm.names.revocable,
            raw_native!(ProxyConstructor::revocable),
            2,
            attributes,
            None,
        );

        object.define_direct_property(
            vm,
            &vm.names.length,
            Value::from_i32(2),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 28.2.1.1 Proxy ( target, handler ), https://tc39.es/ecma262/#sec-proxy-target-handler
    fn call(_: &NativeFunction, vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. If NewTarget is undefined, throw a TypeError exception.
        vm.throw_completion(
            ErrorKind::TypeError,
            ErrorType::ConstructorWithoutNew,
            &[&vm.names.Proxy],
        )
    }

    // 28.2.1.1 Proxy ( target, handler ), https://tc39.es/ecma262/#sec-proxy-target-handler
    fn construct(_: &NativeFunction, vm: &Vm, _new_target: Gc<FunctionObject>) -> ThrowCompletionOr<Gc<Object>> {
        let target = vm.argument(0);
        let handler = vm.argument(1);

        // 2. Return ? ProxyCreate(target, handler).
        Ok(proxy_create(vm, target, handler)?.upcast())
    }

    // 28.2.2.1 Proxy.revocable ( target, handler ), https://tc39.es/ecma262/#sec-proxy.revocable
    fn revocable(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a native function runs in a realm");
        let target = vm.argument(0);
        let handler = vm.argument(1);

        // 1. Let p be ? ProxyCreate(target, handler).
        let proxy = proxy_create(vm, target, handler)?;

        // 2. Let revokerClosure be a new Abstract Closure with no parameters that captures nothing and performs the following steps when called:
        let revoker_closure = |_: &Vm, proxy: &Gc<ProxyObject>| -> ThrowCompletionOr<Value> {
            // a. Let F be the active function object.

            // b. Let p be F.[[RevocableProxy]].
            // c. If p is null, return undefined.
            if proxy.is_revoked() {
                return Ok(Value::UNDEFINED);
            }

            // d. Set F.[[RevocableProxy]] to null.
            // e. Assert: p is a Proxy object.
            // f. Set p.[[ProxyTarget]] to null.
            // g. Set p.[[ProxyHandler]] to null.
            proxy.revoke();

            // h. Return undefined.
            Ok(Value::UNDEFINED)
        };

        // 3. Let revoker be CreateBuiltinFunction(revokerClosure, 0, "", « [[RevocableProxy]] »).
        // 4. Set revoker.[[RevocableProxy]] to p.
        let revoker = NativeFunction::create(
            vm,
            proxy,
            revoker_closure,
            0,
            &PropertyKey::from(Utf16FlyString::default()),
            None,
            None,
            None,
        );

        // 5. Let result be OrdinaryObjectCreate(%Object.prototype%).
        let result = Object::create(vm, realm, Some(realm.object_prototype()));

        // 6. Perform ! CreateDataPropertyOrThrow(result, "proxy", p).
        result
            .create_data_property_or_throw(vm, &vm.names.proxy, Value::from_object(proxy))
            .must();

        // 7. Perform ! CreateDataPropertyOrThrow(result, "revoke", revoker).
        result
            .create_data_property_or_throw(vm, &vm.names.revoke, Value::from_object(revoker))
            .must();

        // 8. Return result.
        Ok(Value::from_object(result))
    }
}
