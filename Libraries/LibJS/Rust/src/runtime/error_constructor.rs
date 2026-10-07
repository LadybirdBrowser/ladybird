/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::get_prototype_from_constructor;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::error::{
    Error, EvalError, InternalError, RangeError, ReferenceError, SyntaxError, TypeError, URIError,
};
use crate::runtime::function_object::FunctionObject;
use crate::runtime::intrinsics::Intrinsics;
use crate::runtime::native_function::{NativeFunction, define_native_function_class, raw_native};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::realm::Realm;

#[repr(C)]
#[derive(Trace)]
pub struct ErrorConstructor {
    base: NativeFunction,
}

define_native_function_class!(
    ErrorConstructor,
    initialize: ErrorConstructor::initialize,
    call: ErrorConstructor::call,
    construct: ErrorConstructor::construct
);

impl ErrorConstructor {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<ErrorConstructor> {
        realm.create_object(
            vm,
            ErrorConstructor {
                base: NativeFunction::new_with_name(
                    vm,
                    Self::CLASS,
                    vm.names.Error.as_string().clone(),
                    realm.function_prototype(),
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        // 20.5.2.1 Error.prototype, https://tc39.es/ecma262/#sec-error.prototype
        object.define_direct_property(
            vm,
            &vm.names.prototype,
            Value::from_object(realm.intrinsics().error_prototype(vm)),
            PropertyAttributes::new(0),
        );

        object.define_direct_property(
            vm,
            &vm.names.length,
            Value::from_i32(1),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );

        let attributes = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        object.define_native_function(
            vm,
            realm,
            &vm.names.isError,
            raw_native!(ErrorConstructor::is_error),
            1,
            attributes,
            None,
        );
    }

    // 20.5.1.1 Error ( message [ , options ] ), https://tc39.es/ecma262/#sec-error-message
    fn call(function: &NativeFunction, vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. If NewTarget is undefined, let newTarget be the active function object; else let newTarget be NewTarget.
        Ok(Value::from_object(Self::construct(
            function,
            vm,
            function.as_function_object_gc(),
        )?))
    }

    // 20.5.1.1 Error ( message [ , options ] ), https://tc39.es/ecma262/#sec-error-message
    fn construct(_: &NativeFunction, vm: &Vm, new_target: Gc<FunctionObject>) -> ThrowCompletionOr<Gc<Object>> {
        let realm = vm.current_realm().expect("a constructor runs in a realm");

        let message = vm.argument(0);
        let options = vm.argument(1);

        // 2. Let O be ? OrdinaryCreateFromConstructor(newTarget, "%Error.prototype%", « [[ErrorData]] »).
        let prototype = get_prototype_from_constructor(vm, new_target, Intrinsics::error_prototype)?;
        let error = realm.create_object(vm, Error::new(vm, Error::CLASS, prototype));

        // 3. If message is not undefined, then
        if !message.is_undefined() {
            // a. Let msg be ? ToString(message).
            let msg = message.to_utf16_string(vm)?;

            // b. Perform CreateNonEnumerableDataPropertyOrThrow(O, "message", msg).
            error.create_non_enumerable_data_property_or_throw(
                vm,
                &vm.names.message,
                Value::from_string(PrimitiveString::create(vm, msg)),
            );
        }

        // 4. Perform ? InstallErrorCause(O, options).
        error.install_error_cause(vm, options)?;

        // 5. Return O.
        Ok(error.upcast())
    }

    // 20.5.2.1 Error.isError ( arg ), https://tc39.es/ecma262/#sec-error.iserror
    #[allow(clippy::unnecessary_wraps, reason = "native functions can throw")]
    fn is_error(vm: &Vm) -> ThrowCompletionOr<Value> {
        let argument = vm.argument(0);

        // 1. If arg is not an Object, return false.
        if !argument.is_object() {
            return Ok(Value::FALSE);
        }

        // 2. If arg does not have an [[ErrorData]] internal slot, return false.
        if !argument.as_object().has_error_data() {
            return Ok(Value::FALSE);
        }

        // 3. Return true.
        Ok(Value::TRUE)
    }
}

macro_rules! define_native_error_constructors {
    ($($constructor:ident: $class:ident, $name:ident, $prototype:ident;)*) => {
        $(
            #[repr(C)]
            #[derive(Trace)]
            pub struct $constructor {
                base: NativeFunction,
            }

            define_native_function_class!(
                $constructor,
                initialize: $constructor::initialize,
                call: $constructor::call,
                construct: $constructor::construct
            );

            impl $constructor {
                pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<$constructor> {
                    realm.create_object(
                        vm,
                        $constructor {
                            base: NativeFunction::new_with_name(
                                vm,
                                Self::CLASS,
                                vm.names.$name.as_string().clone(),
                                realm.intrinsics().error_constructor(vm).upcast(),
                            ),
                        },
                    )
                }

                fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
                    // 20.5.6.2.1 NativeError.prototype, https://tc39.es/ecma262/#sec-nativeerror.prototype
                    object.define_direct_property(
                        vm,
                        &vm.names.prototype,
                        Value::from_object(realm.intrinsics().$prototype(vm)),
                        PropertyAttributes::new(0),
                    );

                    object.define_direct_property(
                        vm,
                        &vm.names.length,
                        Value::from_i32(1),
                        PropertyAttributes::new(Attribute::CONFIGURABLE),
                    );
                }

                // 20.5.6.1.1 NativeError ( message [ , options ] ), https://tc39.es/ecma262/#sec-nativeerror
                fn call(function: &NativeFunction, vm: &Vm) -> ThrowCompletionOr<Value> {
                    // 1. If NewTarget is undefined, let newTarget be the active function object; else let newTarget be NewTarget.
                    Ok(Value::from_object(Self::construct(
                        function,
                        vm,
                        function.as_function_object_gc(),
                    )?))
                }

                // 20.5.6.1.1 NativeError ( message [ , options ] ), https://tc39.es/ecma262/#sec-nativeerror
                fn construct(
                    _: &NativeFunction,
                    vm: &Vm,
                    new_target: Gc<FunctionObject>,
                ) -> ThrowCompletionOr<Gc<Object>> {
                    let realm = vm.current_realm().expect("a constructor runs in a realm");

                    let message = vm.argument(0);
                    let options = vm.argument(1);

                    // 2. Let O be ? OrdinaryCreateFromConstructor(newTarget, "%NativeError.prototype%", « [[ErrorData]] »).
                    let prototype = get_prototype_from_constructor(vm, new_target, Intrinsics::$prototype)?;
                    let error = realm.create_object(vm, $class::new(vm, prototype));

                    // 3. If message is not undefined, then
                    if !message.is_undefined() {
                        // a. Let msg be ? ToString(message).
                        let msg = message.to_utf16_string(vm)?;

                        // b. Perform CreateNonEnumerableDataPropertyOrThrow(O, "message", msg).
                        error.create_non_enumerable_data_property_or_throw(
                            vm,
                            &vm.names.message,
                            Value::from_string(PrimitiveString::create(vm, msg)),
                        );
                    }

                    // 4. Perform ? InstallErrorCause(O, options).
                    error.install_error_cause(vm, options)?;

                    // 5. Return O.
                    Ok(error.upcast())
                }
            }
        )*
    };
}

define_native_error_constructors! {
    EvalErrorConstructor: EvalError, EvalError, eval_error_prototype;
    InternalErrorConstructor: InternalError, InternalError, internal_error_prototype;
    RangeErrorConstructor: RangeError, RangeError, range_error_prototype;
    ReferenceErrorConstructor: ReferenceError, ReferenceError, reference_error_prototype;
    SyntaxErrorConstructor: SyntaxError, SyntaxError, syntax_error_prototype;
    TypeErrorConstructor: TypeError, TypeError, type_error_prototype;
    URIErrorConstructor: URIError, URIError, uri_error_prototype;
}
