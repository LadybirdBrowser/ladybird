/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use ak::{Utf16FlyString, Utf16String};
use libjs_runtime_macros::Trace;

use crate::bytecode::executable::StaticPropertyLookupCacheSite;
use crate::gc::class::{GcCell, define_cell};
use crate::gc::class_id::ClassId;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::layout_forward::RawNativeFunctionPointer;
use crate::runtime::abstract_operations::require_object_coercible;
use crate::runtime::boolean_object::BooleanObject;
use crate::runtime::completion::{Must, ThrowCompletionOr};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::function_object::FunctionObject;
use crate::runtime::native_function::raw_native;
use crate::runtime::number_object::NumberObject;
use crate::runtime::object::{MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, define_object_class};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_descriptor::PropertyDescriptor;
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;
use crate::runtime::regexp_constructor::is_raw_native_function_running;
use crate::runtime::string_object::StringObject;
use crate::runtime::value::same_value;
use crate::utf16::{Utf16View, concatenate};

/// %Object.prototype.valueOf%, which is_value_of_function() looks for.
static VALUE_OF_FUNCTION: RawNativeFunctionPointer = raw_native!(ObjectPrototype::value_of);

/// %Object.prototype%, an immutable prototype exotic object.
#[repr(C)]
#[derive(Trace)]
pub struct ObjectPrototype {
    base: Object,
}

define_object_class!(ObjectPrototype, extends: [Object], methods: {
    initialize: ObjectPrototype::initialize,
    internal_set_prototype_of: ObjectPrototype::internal_set_prototype_of,
    ..ORDINARY_OBJECT_METHODS
});

/// The longest tag whose Object.prototype.toString result is made without building a string first.
const SHORT_TAG_LENGTH: usize = 23;

/// Whether no object on the prototype chain of `object`, starting with `object`, has a @@toStringTag property, and all of
/// them look properties up in their storage, so that Get of @@toStringTag gives undefined without running code.
fn has_no_to_string_tag_on_prototype_chain(vm: &Vm, object: &Object) -> bool {
    const MAX_PROTOTYPE_CHAIN_LENGTH: usize = 8;
    let object_prototype = vm.current_realm().map(|realm| realm.object_prototype());
    let mut current = Some(object.as_gc());
    for _ in 0..MAX_PROTOTYPE_CHAIN_LENGTH {
        let Some(object) = current else {
            return true;
        };
        // NB: %Object.prototype% only has an exotic [[SetPrototypeOf]]. Intrinsic accessors, whose keys are in the shape
        //     already, only have string keys.
        let is_ordinary = Some(object) == object_prototype || object.has_ordinary_named_property_lookup();
        if !is_ordinary || object.shape().has_to_string_tag(vm) {
            return false;
        }
        current = object.shape().prototype();
    }
    false
}

/// Steps 5 to 14 of Object.prototype.toString: the builtinTag of `object`, given whether IsArray(object) is true.
pub fn builtin_tag(object: &Object, is_array: bool) -> &'static str {
    // 5. If isArray is true, let builtinTag be "Array".
    if is_array {
        "Array"
    }
    // 6. Else if O has a [[ParameterMap]] internal slot, let builtinTag be "Arguments".
    else if object.has_parameter_map() {
        "Arguments"
    }
    // 7. Else if O has a [[Call]] internal method, let builtinTag be "Function".
    else if object.is_function() {
        "Function"
    }
    // 8. Else if O has an [[ErrorData]] internal slot, let builtinTag be "Error".
    else if object.has_error_data() {
        "Error"
    }
    // 9. Else if O has a [[BooleanData]] internal slot, let builtinTag be "Boolean".
    else if object.is::<BooleanObject>() {
        "Boolean"
    }
    // 10. Else if O has a [[NumberData]] internal slot, let builtinTag be "Number".
    else if object.is::<NumberObject>() {
        "Number"
    }
    // 11. Else if O has a [[StringData]] internal slot, let builtinTag be "String".
    else if object.is::<StringObject>() {
        "String"
    }
    // 12. Else if O has a [[DateValue]] internal slot, let builtinTag be "Date".
    else if object.class().id == ClassId::Date {
        "Date"
    }
    // 13. Else if O has a [[RegExpMatcher]] internal slot, let builtinTag be "RegExp".
    else if object.class().id == ClassId::RegExpObject {
        "RegExp"
    }
    // 14. Else, let builtinTag be "Object".
    else {
        "Object"
    }
}

impl ObjectPrototype {
    pub fn new(vm: &Vm, realm: Gc<Realm>) -> ObjectPrototype {
        ObjectPrototype {
            base: Object::new_without_prototype(vm, Self::CLASS, realm, MayInterfereWithIndexedPropertyAccess::No),
        }
    }

    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<ObjectPrototype> {
        realm.create_object(vm, Self::new(vm, realm))
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        let names = &vm.names;

        // This must be called after the constructor has returned, so that the below code
        // can find the ObjectPrototype through normal paths.
        let attr = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        let define_native_function = |property_key: &PropertyKey, function, length| {
            object.define_native_function(vm, realm, property_key, function, length, attr, None);
        };
        define_native_function(&names.hasOwnProperty, raw_native!(ObjectPrototype::has_own_property), 1);
        define_native_function(&names.toString, raw_native!(ObjectPrototype::to_string), 0);
        define_native_function(&names.toLocaleString, raw_native!(ObjectPrototype::to_locale_string), 0);
        define_native_function(&names.valueOf, VALUE_OF_FUNCTION, 0);
        define_native_function(
            &names.propertyIsEnumerable,
            raw_native!(ObjectPrototype::property_is_enumerable),
            1,
        );
        define_native_function(&names.isPrototypeOf, raw_native!(ObjectPrototype::is_prototype_of), 1);

        // Annex B
        define_native_function(&names.__defineGetter__, raw_native!(ObjectPrototype::define_getter), 2);
        define_native_function(&names.__defineSetter__, raw_native!(ObjectPrototype::define_setter), 2);
        define_native_function(&names.__lookupGetter__, raw_native!(ObjectPrototype::lookup_getter), 1);
        define_native_function(&names.__lookupSetter__, raw_native!(ObjectPrototype::lookup_setter), 1);
        object.define_native_accessor(
            vm,
            realm,
            &names.__proto__,
            raw_native!(ObjectPrototype::proto_getter),
            raw_native!(ObjectPrototype::proto_setter),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 10.4.7.1 [[SetPrototypeOf]] ( V ), https://tc39.es/ecma262/#sec-immutable-prototype-exotic-objects-setprototypeof-v
    fn internal_set_prototype_of(object: &Object, vm: &Vm, prototype: Option<Gc<Object>>) -> ThrowCompletionOr<bool> {
        // 1. Return ? SetImmutablePrototype(O, V).
        object.set_immutable_prototype(vm, prototype)
    }

    // 20.1.3.2 Object.prototype.hasOwnProperty ( V ), https://tc39.es/ecma262/#sec-object.prototype.hasownproperty
    fn has_own_property(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let P be ? ToPropertyKey(V).
        let property_key = vm.argument(0).to_property_key(vm)?;

        // 2. Let O be ? ToObject(this value).
        let this_object = vm.this_value().to_object(vm)?;

        // 3. Return ? HasOwnProperty(O, P).
        Ok(Value::from_bool(this_object.has_own_property(vm, &property_key)?))
    }

    // 20.1.3.3 Object.prototype.isPrototypeOf ( V ), https://tc39.es/ecma262/#sec-object.prototype.isprototypeof
    fn is_prototype_of(vm: &Vm) -> ThrowCompletionOr<Value> {
        let object_argument = vm.argument(0);

        // 1. If V is not an Object, return false.
        if !object_argument.is_object() {
            return Ok(Value::FALSE);
        }
        let mut object = object_argument.as_object();

        // 2. Let O be ? ToObject(this value).
        let this_object = vm.this_value().to_object(vm)?;

        // 3. Repeat,
        loop {
            // a. Set V to ? V.[[GetPrototypeOf]]().
            let prototype = object.internal_get_prototype_of(vm)?;

            // b. If V is null, return false.
            let Some(prototype) = prototype else {
                return Ok(Value::FALSE);
            };
            object = prototype;

            // c. If SameValue(O, V) is true, return true.
            if same_value(Value::from_object(this_object), Value::from_object(object)) {
                return Ok(Value::TRUE);
            }
        }
    }

    // 20.1.3.4 Object.prototype.propertyIsEnumerable ( V ), https://tc39.es/ecma262/#sec-object.prototype.propertyisenumerable
    fn property_is_enumerable(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let P be ? ToPropertyKey(V).
        let property_key = vm.argument(0).to_property_key(vm)?;

        // 2. Let O be ? ToObject(this value).
        let this_object = vm.this_value().to_object(vm)?;

        // 3. Let desc be ? O.[[GetOwnProperty]](P).
        let property_descriptor = this_object.internal_get_own_property(vm, &property_key)?;

        // 4. If desc is undefined, return false.
        let Some(property_descriptor) = property_descriptor else {
            return Ok(Value::FALSE);
        };

        // 5. Return desc.[[Enumerable]].
        Ok(Value::from_bool(
            property_descriptor
                .enumerable
                .expect("an own property descriptor is complete"),
        ))
    }

    // 20.1.3.5 Object.prototype.toLocaleString ( [ reserved1 [ , reserved2 ] ] ), https://tc39.es/ecma262/#sec-object.prototype.tolocalestring
    fn to_locale_string(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let O be the this value.
        let this_value = vm.this_value();

        // 2. Return ? Invoke(O, "toString").
        this_value.invoke(vm, &vm.names.toString, &[])
    }

    // 20.1.3.6 Object.prototype.toString ( ), https://tc39.es/ecma262/#sec-object.prototype.tostring
    fn to_string(vm: &Vm) -> ThrowCompletionOr<Value> {
        let this_value = vm.this_value();

        // 1. If the this value is undefined, return "[object Undefined]".
        if this_value.is_undefined() {
            return Ok(Value::from_string(PrimitiveString::create_from_fly_string(
                vm,
                &Utf16FlyString::from_utf8("[object Undefined]"),
            )));
        }

        // 2. If the this value is null, return "[object Null]".
        if this_value.is_null() {
            return Ok(Value::from_string(PrimitiveString::create_from_fly_string(
                vm,
                &Utf16FlyString::from_utf8("[object Null]"),
            )));
        }

        // 3. Let O be ! ToObject(this value).
        let object = this_value.to_object(vm).must();

        // 4. Let isArray be ? IsArray(O).
        let is_array = Value::from_object(object).is_array(vm)?;

        // NB: Steps 5 to 14 are in builtin_tag().
        let builtin_tag = builtin_tag(&object, is_array);

        // 15. Let tag be ? Get(O, @@toStringTag).
        // OPTIMIZATION: Get finds nothing without running code when no object on the chain has the property.
        let to_string_tag = if has_no_to_string_tag_on_prototype_chain(vm, &object) {
            Value::UNDEFINED
        } else {
            object.get_with_cache(
                vm,
                &PropertyKey::from(vm.well_known_symbols().to_string_tag),
                vm.static_property_lookup_cache(StaticPropertyLookupCacheSite::ObjectPrototypeToStringToStringTag),
            )?
        };

        // Optimization: Instead of creating another PrimitiveString from builtin_tag, we separate tag and to_string_tag and add an additional branch to step 16.
        let custom_tag: Utf16String;

        // 16. If Type(tag) is not String, set tag to builtinTag.
        let tag = if !to_string_tag.is_string() {
            Utf16View::Ascii(builtin_tag.as_bytes())
        } else {
            custom_tag = to_string_tag.as_string().utf16_string();
            Utf16View::of_string(&custom_tag)
        };

        // 17. Return the string-concatenation of "[object ", tag, and "]".

        // OPTIMIZATION: The VM has a cache for the extremely common "[object Object]" string.
        if tag == "Object" {
            return Ok(Value::from_string(vm.cached_strings().object_Object));
        }
        // OPTIMIZATION: The result for a short ASCII tag is made from its characters without building a string first.
        if let Utf16View::Ascii(tag_characters) = tag
            && tag_characters.len() <= SHORT_TAG_LENGTH
        {
            let mut characters = [0; SHORT_TAG_LENGTH + 9];
            characters[..8].copy_from_slice(b"[object ");
            characters[8..8 + tag_characters.len()].copy_from_slice(tag_characters);
            characters[8 + tag_characters.len()] = b']';
            return Ok(Value::from_string(PrimitiveString::create_from_ascii(
                vm,
                &characters[..tag_characters.len() + 9],
            )));
        }
        Ok(Value::from_string(PrimitiveString::create(
            vm,
            concatenate(&[Utf16View::Ascii(b"[object "), tag, Utf16View::Ascii(b"]")]),
        )))
    }

    /// Whether `function` is %Object.prototype.valueOf% of some realm.
    pub fn is_value_of_function(vm: &Vm, function: Gc<FunctionObject>) -> bool {
        is_raw_native_function_running(vm, function, VALUE_OF_FUNCTION)
    }

    // 20.1.3.7 Object.prototype.valueOf ( ), https://tc39.es/ecma262/#sec-object.prototype.valueof
    fn value_of(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Return ? ToObject(this value).
        Ok(Value::from_object(vm.this_value().to_object(vm)?))
    }

    // 20.1.3.8.1 get Object.prototype.__proto__, https://tc39.es/ecma262/#sec-get-object.prototype.__proto__
    fn proto_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let O be ? ToObject(this value).
        let object = vm.this_value().to_object(vm)?;

        // 2. Return ? O.[[GetPrototypeOf]]().
        Ok(object
            .internal_get_prototype_of(vm)?
            .map_or(Value::NULL, Value::from_object))
    }

    // 20.1.3.8.2 set Object.prototype.__proto__, https://tc39.es/ecma262/#sec-set-object.prototype.__proto__
    fn proto_setter(vm: &Vm) -> ThrowCompletionOr<Value> {
        let prototype = vm.argument(0);

        // 1. Let O be ? RequireObjectCoercible(this value).
        let object = require_object_coercible(vm, vm.this_value())?;

        // 2. If proto is not an Object and proto is not null, return undefined.
        if !prototype.is_object() && !prototype.is_null() {
            return Ok(Value::UNDEFINED);
        }

        // 3. If O is not an Object, return undefined.
        if !object.is_object() {
            return Ok(Value::UNDEFINED);
        }

        // 4. Let status be ? O.[[SetPrototypeOf]](proto).
        let status = object
            .as_object()
            .internal_set_prototype_of(vm, prototype.is_object().then(|| prototype.as_object()))?;

        // 5. If status is false, throw a TypeError exception.
        if !status {
            // FIXME: Improve/contextualize error message
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::ObjectSetPrototypeOfReturnedFalse, &[]);
        }

        // 6. Return undefined.
        Ok(Value::UNDEFINED)
    }

    // 20.1.3.9.1 Object.prototype.__defineGetter__ ( P, getter ), https://tc39.es/ecma262/#sec-object.prototype.__defineGetter__
    fn define_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        let property = vm.argument(0);
        let getter = vm.argument(1);

        // 1. Let O be ? ToObject(this value).
        let object = vm.this_value().to_object(vm)?;

        // 2. If IsCallable(getter) is false, throw a TypeError exception.
        if !getter.is_function() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAFunction, &[&getter]);
        }

        // 3. Let desc be PropertyDescriptor { [[Get]]: getter, [[Enumerable]]: true, [[Configurable]]: true }.
        let mut descriptor = PropertyDescriptor {
            get: Some(Some(getter.as_function())),
            enumerable: Some(true),
            configurable: Some(true),
            ..Default::default()
        };

        // 4. Let key be ? ToPropertyKey(P).
        let key = property.to_property_key(vm)?;

        // 5. Perform ? DefinePropertyOrThrow(O, key, desc).
        object.define_property_or_throw(vm, &key, &mut descriptor)?;

        // 6. Return undefined.
        Ok(Value::UNDEFINED)
    }

    // 20.1.3.9.2 Object.prototype.__defineSetter__ ( P, setter ), https://tc39.es/ecma262/#sec-object.prototype.__defineSetter__
    fn define_setter(vm: &Vm) -> ThrowCompletionOr<Value> {
        let property = vm.argument(0);
        let setter = vm.argument(1);

        // 1. Let O be ? ToObject(this value).
        let object = vm.this_value().to_object(vm)?;

        // 2. If IsCallable(setter) is false, throw a TypeError exception.
        if !setter.is_function() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAFunction, &[&setter]);
        }

        // 3. Let desc be PropertyDescriptor { [[Set]]: setter, [[Enumerable]]: true, [[Configurable]]: true }.
        let mut descriptor = PropertyDescriptor {
            set: Some(Some(setter.as_function())),
            enumerable: Some(true),
            configurable: Some(true),
            ..Default::default()
        };

        // 4. Let key be ? ToPropertyKey(P).
        let key = property.to_property_key(vm)?;

        // 5. Perform ? DefinePropertyOrThrow(O, key, desc).
        object.define_property_or_throw(vm, &key, &mut descriptor)?;

        // 6. Return undefined.
        Ok(Value::UNDEFINED)
    }

    // 20.1.3.9.3 Object.prototype.__lookupGetter__ ( P ), https://tc39.es/ecma262/#sec-object.prototype.__lookupGetter__
    fn lookup_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        let property = vm.argument(0);

        // 1. Let O be ? ToObject(this value).
        let mut object = Some(vm.this_value().to_object(vm)?);

        // 2. Let key be ? ToPropertyKey(P).
        let key = property.to_property_key(vm)?;

        // 3. Repeat,
        while let Some(current) = object {
            // a. Let desc be ? O.[[GetOwnProperty]](key).
            let descriptor = current.internal_get_own_property(vm, &key)?;

            // b. If desc is not undefined, then
            if let Some(descriptor) = descriptor {
                // i. If IsAccessorDescriptor(desc) is true, return desc.[[Get]].
                if descriptor.is_accessor_descriptor() {
                    return Ok(descriptor
                        .get
                        .expect("an own accessor property descriptor is complete")
                        .map_or(Value::UNDEFINED, Value::from_object));
                }

                // ii. Return undefined.
                return Ok(Value::UNDEFINED);
            }

            // c. Set O to ? O.[[GetPrototypeOf]]().
            object = current.internal_get_prototype_of(vm)?;
        }

        // d. If O is null, return undefined.
        Ok(Value::UNDEFINED)
    }

    // 20.1.3.9.4 Object.prototype.__lookupSetter__ ( P ), https://tc39.es/ecma262/#sec-object.prototype.__lookupSetter__
    fn lookup_setter(vm: &Vm) -> ThrowCompletionOr<Value> {
        let property = vm.argument(0);

        // 1. Let O be ? ToObject(this value).
        let mut object = Some(vm.this_value().to_object(vm)?);

        // 2. Let key be ? ToPropertyKey(P).
        let key = property.to_property_key(vm)?;

        // 3. Repeat,
        while let Some(current) = object {
            // a. Let desc be ? O.[[GetOwnProperty]](key).
            let descriptor = current.internal_get_own_property(vm, &key)?;

            // b. If desc is not undefined, then
            if let Some(descriptor) = descriptor {
                // i. If IsAccessorDescriptor(desc) is true, return desc.[[Set]].
                if descriptor.is_accessor_descriptor() {
                    return Ok(descriptor
                        .set
                        .expect("an own accessor property descriptor is complete")
                        .map_or(Value::UNDEFINED, Value::from_object));
                }

                // ii. Return undefined.
                return Ok(Value::UNDEFINED);
            }

            // c. Set O to ? O.[[GetPrototypeOf]]().
            object = current.internal_get_prototype_of(vm)?;
        }

        // d. If O is null, return undefined.
        Ok(Value::UNDEFINED)
    }
}
