/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use ak::{Utf16FlyString, Utf16String};
use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::error::ErrorKind;
use crate::runtime::error_data::CompactTraceback;
use crate::runtime::error_types::ErrorType;
use crate::runtime::native_function::raw_native;
use crate::runtime::object::{MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, define_object_class};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::prototype_object::this_object;
use crate::runtime::realm::Realm;
use crate::utf16::{Utf16View, concatenate};

#[repr(C)]
#[derive(Trace)]
pub struct ErrorPrototype {
    base: Object,
}

define_object_class!(ErrorPrototype, extends: [Object], methods: {
    initialize: ErrorPrototype::initialize,
    ..ORDINARY_OBJECT_METHODS
});

impl ErrorPrototype {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<ErrorPrototype> {
        realm.create_object(
            vm,
            ErrorPrototype {
                base: Object::new_with_prototype(
                    vm,
                    Self::CLASS,
                    realm.object_prototype(),
                    MayInterfereWithIndexedPropertyAccess::No,
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        let names = &vm.names;
        let attributes = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        object.define_direct_property(
            vm,
            &names.name,
            Value::from_string(PrimitiveString::create_from_fly_string(
                vm,
                &Utf16FlyString::from_utf8("Error"),
            )),
            attributes,
        );
        object.define_direct_property(
            vm,
            &names.message,
            Value::from_string(PrimitiveString::create(vm, Utf16String::default())),
            attributes,
        );
        object.define_native_function(
            vm,
            realm,
            &names.toString,
            raw_native!(ErrorPrototype::to_string),
            0,
            attributes,
            None,
        );
        // Non standard property "stack"
        // Every other engine seems to have this in some way or another, and the spec
        // proposal for this is only Stage 1
        object.define_native_accessor(
            vm,
            realm,
            &names.stack,
            raw_native!(ErrorPrototype::stack_getter),
            raw_native!(ErrorPrototype::stack_setter),
            attributes,
        );
    }

    // 20.5.3.4 Error.prototype.toString ( ), https://tc39.es/ecma262/#sec-error.prototype.tostring
    fn to_string(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let O be the this value.
        // 2. If Type(O) is not Object, throw a TypeError exception.
        let this_object = this_object(vm)?;

        // 3. Let name be ? Get(O, "name").
        let name_property = this_object.get(vm, &vm.names.name)?;

        // 4. If name is undefined, set name to "Error"; otherwise set name to ? ToString(name).
        let name = if name_property.is_undefined() {
            Utf16String::from_utf8("Error")
        } else {
            name_property.to_utf16_string(vm)?
        };

        // 5. Let msg be ? Get(O, "message").
        let message_property = this_object.get(vm, &vm.names.message)?;

        // 6. If msg is undefined, set msg to the empty String; otherwise set msg to ? ToString(msg).
        let message = if message_property.is_undefined() {
            Utf16String::default()
        } else {
            message_property.to_utf16_string(vm)?
        };

        // 7. If name is the empty String, return msg.
        if Utf16View::of_string(&name).is_empty() {
            return Ok(Value::from_string(PrimitiveString::create(vm, message)));
        }

        // 8. If msg is the empty String, return name.
        if Utf16View::of_string(&message).is_empty() {
            return Ok(Value::from_string(PrimitiveString::create(vm, name)));
        }

        // 9. Return the string-concatenation of name, the code unit 0x003A (COLON), the code unit 0x0020 (SPACE), and msg.
        Ok(Value::from_string(PrimitiveString::create(
            vm,
            concatenate(&[
                Utf16View::of_string(&name),
                Utf16View::Ascii(b": "),
                Utf16View::of_string(&message),
            ]),
        )))
    }

    // B.1.1 get Error.prototype.stack ( ), https://tc39.es/proposal-error-stacks/#sec-get-error.prototype-stack
    fn stack_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let E be the this value.
        // 2. If ! Type(E) is not Object, throw a TypeError exception.
        let this_object = this_object(vm)?;

        // 3. If E does not have an [[ErrorData]] internal slot, return undefined.
        let Some(error_data) = this_object.error_data() else {
            return Ok(Value::UNDEFINED);
        };

        // OPTIMIZATION: Avoid recomputing the stack string if we already have it cached.
        //               At least one major engine does this as well, so it's not expected that changing
        //               the name or message properties updates the stack string.
        if let Some(cached_string) = error_data.cached_string() {
            return Ok(Value::from_string(cached_string));
        }

        // 4. Return ? GetStackString(error).
        // NOTE: These steps are not implemented based on the proposal, but to roughly follow behavior of other browsers.

        let name_property = this_object.get(vm, &vm.names.name)?;
        let name = if !name_property.is_undefined() {
            name_property.to_utf16_string(vm)?
        } else {
            Utf16String::from_utf8("Error")
        };

        let message_property = this_object.get(vm, &vm.names.message)?;
        let message = if !message_property.is_undefined() {
            message_property.to_utf16_string(vm)?
        } else {
            Utf16String::default()
        };

        let header = if Utf16View::of_string(&message).is_empty() {
            name
        } else {
            concatenate(&[
                Utf16View::of_string(&name),
                Utf16View::Ascii(b": "),
                Utf16View::of_string(&message),
            ])
        };

        let stack_string = error_data.stack_string(CompactTraceback::No);
        let string = PrimitiveString::create(
            vm,
            concatenate(&[
                Utf16View::of_string(&header),
                Utf16View::Ascii(b"\n"),
                Utf16View::of_string(&stack_string),
            ]),
        );
        error_data.set_cached_string(string);
        Ok(Value::from_string(string))
    }

    // B.1.2 set Error.prototype.stack ( value ), https://tc39.es/proposal-error-stacks/#sec-set-error.prototype-stack
    fn stack_setter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let E be the this value.
        let this_value = vm.this_value();

        // 2. If ! Type(E) is not Object, throw a TypeError exception.
        if !this_value.is_object() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAnObject, &[&this_value]);
        }

        let this_object = this_value.as_object();

        // 3. Let numberOfArgs be the number of arguments passed to this function call.
        // 4. If numberOfArgs is 0, throw a TypeError exception.
        if vm.argument_count() == 0 {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::BadArgCountOne, &[&"set stack"]);
        }

        // 5. Return ? CreateDataPropertyOrThrow(E, "stack", value);
        Ok(Value::from_bool(this_object.create_data_property_or_throw(
            vm,
            &vm.names.stack,
            vm.argument(0),
        )?))
    }
}

macro_rules! define_native_error_prototypes {
    ($($prototype:ident: $class_name:literal;)*) => {
        $(
            #[repr(C)]
            #[derive(Trace)]
            pub struct $prototype {
                base: Object,
            }

            define_object_class!($prototype, extends: [Object], methods: {
                initialize: $prototype::initialize,
                ..ORDINARY_OBJECT_METHODS
            });

            impl $prototype {
                pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<$prototype> {
                    realm.create_object(
                        vm,
                        $prototype {
                            base: Object::new_with_prototype(
                                vm,
                                Self::CLASS,
                                realm.intrinsics().error_prototype(vm),
                                MayInterfereWithIndexedPropertyAccess::No,
                            ),
                        },
                    )
                }

                fn initialize(object: &Object, vm: &Vm, _realm: Gc<Realm>) {
                    let attributes = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
                    object.define_direct_property(
                        vm,
                        &vm.names.name,
                        Value::from_string(PrimitiveString::create_from_fly_string(
                            vm,
                            &Utf16FlyString::from_utf8($class_name),
                        )),
                        attributes,
                    );
                    object.define_direct_property(
                        vm,
                        &vm.names.message,
                        Value::from_string(PrimitiveString::create(vm, Utf16String::default())),
                        attributes,
                    );
                }
            }
        )*
    };
}

define_native_error_prototypes! {
    EvalErrorPrototype: "EvalError";
    InternalErrorPrototype: "InternalError";
    RangeErrorPrototype: "RangeError";
    ReferenceErrorPrototype: "ReferenceError";
    SyntaxErrorPrototype: "SyntaxError";
    TypeErrorPrototype: "TypeError";
    URIErrorPrototype: "URIError";
}
