/*
 * Copyright (c) 2020-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Function.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/ReflectObject.h>

namespace JS {

JS_DEFINE_ALLOCATOR(ReflectObject);

ReflectObject::ReflectObject(Realm& realm)
    : Object(ConstructWithPrototypeTag::Tag, realm.intrinsics().object_prototype())
{
}

void ReflectObject::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);
    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.apply, apply, 3, attr);
    define_native_function(realm, vm.names.construct, construct, 2, attr);
    define_native_function(realm, vm.names.defineProperty, define_property, 3, attr);
    define_native_function(realm, vm.names.deleteProperty, delete_property, 2, attr);
    define_native_function(realm, vm.names.get, get, 2, attr);
    define_native_function(realm, vm.names.getOwnPropertyDescriptor, get_own_property_descriptor, 2, attr);
    define_native_function(realm, vm.names.getPrototypeOf, get_prototype_of, 1, attr);
    define_native_function(realm, vm.names.has, has, 2, attr);
    define_native_function(realm, vm.names.isExtensible, is_extensible, 1, attr);
    define_native_function(realm, vm.names.ownKeys, own_keys, 1, attr);
    define_native_function(realm, vm.names.preventExtensions, prevent_extensions, 1, attr);
    define_native_function(realm, vm.names.set, set, 3, attr);
    define_native_function(realm, vm.names.setPrototypeOf, set_prototype_of, 2, attr);

    // 28.1.14 Reflect [ @@toStringTag ], https://tc39.es/ecma262/#sec-reflect-@@tostringtag
    define_direct_property(vm.well_known_symbol_to_string_tag(), PrimitiveString::create(vm, vm.names.Reflect.as_string()), Attribute::Configurable);
}

// 28.1.1 Reflect.apply ( target, thisArgument, argumentsList ), https://tc39.es/ecma262/#sec-reflect.apply
JS_DEFINE_NATIVE_FUNCTION(ReflectObject::apply)
{
    auto target = vm.argument(0);
    auto this_argument = vm.argument(1);
    auto arguments_list = vm.argument(2);

    // 1. If IsCallable(target) is false, throw a TypeError exception.
    if (!target.is_function())
        return vm.throw_completion<TypeError>(ErrorType::NotAFunction, target.to_string_without_side_effects());

    // 2. Let args be ? CreateListFromArrayLike(argumentsList).
    auto args = TRY(create_list_from_array_like(vm, arguments_list));

    // 3. Perform PrepareForTailCall().
    // 4. Return ? Call(target, thisArgument, args).
    return TRY(call(vm, target.as_function(), this_argument, args.span()));
}

// 28.1.2 Reflect.construct ( target, argumentsList [ , newTarget ] ), https://tc39.es/ecma262/#sec-reflect.construct
JS_DEFINE_NATIVE_FUNCTION(ReflectObject::construct)
{
    auto target = vm.argument(0);
    auto arguments_list = vm.argument(1);
    auto new_target = vm.argument(2);

    // 1. If IsConstructor(target) is false, throw a TypeError exception.
    if (!target.is_constructor())
        return vm.throw_completion<TypeError>(ErrorType::NotAConstructor, target.to_string_without_side_effects());

    // 2. If newTarget is not present, set newTarget to target.
    if (vm.argument_count() < 3)
        new_target = target;
    // 3. Else if IsConstructor(newTarget) is false, throw a TypeError exception.
    else if (!new_target.is_constructor())
        return vm.throw_completion<TypeError>(ErrorType::NotAConstructor, new_target.to_string_without_side_effects());

    // 4. Let args be ? CreateListFromArrayLike(argumentsList).
    auto args = TRY(create_list_from_array_like(vm, arguments_list));

    // 5. Return ? Construct(target, args, newTarget).
    return TRY(JS::construct(vm, target.as_function(), args.span(), &new_target.as_function()));
}

// 28.1.3 Reflect.defineProperty ( target, propertyKey, attributes ), https://tc39.es/ecma262/#sec-reflect.defineproperty
JS_DEFINE_NATIVE_FUNCTION(ReflectObject::define_property)
{
    auto target = vm.argument(0);
    auto property_key = vm.argument(1);
    auto attributes = vm.argument(2);

    // 1. If Type(target) is not Object, throw a TypeError exception.
    if (!target.is_object())
        return vm.throw_completion<TypeError>(ErrorType::NotAnObject, target.to_string_without_side_effects());

    // 2. Let key be ? ToPropertyKey(propertyKey).
    auto key = TRY(property_key.to_property_key(vm));

    // 3. Let desc be ? ToPropertyDescriptor(attributes).
    auto descriptor = TRY(to_property_descriptor(vm, attributes));

    // 4. Return ? target.[[DefineOwnProperty]](key, desc).
    return Value(TRY(target.as_object().internal_define_own_property(key, descriptor)));
}

// 28.1.4 Reflect.deleteProperty ( target, propertyKey ), https://tc39.es/ecma262/#sec-reflect.deleteproperty
JS_DEFINE_NATIVE_FUNCTION(ReflectObject::delete_property)
{
    auto target = vm.argument(0);
    auto property_key = vm.argument(1);

    // 1. If Type(target) is not Object, throw a TypeError exception.
    if (!target.is_object())
        return vm.throw_completion<TypeError>(ErrorType::NotAnObject, target.to_string_without_side_effects());

    // 2. Let key be ? ToPropertyKey(propertyKey).
    auto key = TRY(property_key.to_property_key(vm));

    // 3. Return ? target.[[Delete]](key).
    return Value(TRY(target.as_object().internal_delete(key)));
}

// 28.1.5 Reflect.get ( target, propertyKey [ , receiver ] ), https://tc39.es/ecma262/#sec-reflect.get
JS_DEFINE_NATIVE_FUNCTION(ReflectObject::get)
{
    auto target = vm.argument(0);
    auto property_key = vm.argument(1);
    auto receiver = vm.argument(2);

    // 1. If Type(target) is not Object, throw a TypeError exception.
    if (!target.is_object())
        return vm.throw_completion<TypeError>(ErrorType::NotAnObject, target.to_string_without_side_effects());

    // 2. Let key be ? ToPropertyKey(propertyKey).
    auto key = TRY(property_key.to_property_key(vm));

    // 3. If receiver is not present, then
    if (vm.argument_count() < 3) {
        // a. Set receiver to target.
        receiver = target;
    }

    // 4. Return ? target.[[Get]](key, receiver).
    return TRY(target.as_object().internal_get(key, receiver));
}

// 28.1.6 Reflect.getOwnPropertyDescriptor ( target, propertyKey ), https://tc39.es/ecma262/#sec-reflect.getownpropertydescriptor
JS_DEFINE_NATIVE_FUNCTION(ReflectObject::get_own_property_descriptor)
{
    auto target = vm.argument(0);
    auto property_key = vm.argument(1);

    // 1. If Type(target) is not Object, throw a TypeError exception.
    if (!target.is_object())
        return vm.throw_completion<TypeError>(ErrorType::NotAnObject, target.to_string_without_side_effects());

    // 2. Let key be ? ToPropertyKey(propertyKey).
    auto key = TRY(property_key.to_property_key(vm));

    // 3. Let desc be ? target.[[GetOwnProperty]](key).
    auto descriptor = TRY(target.as_object().internal_get_own_property(key));

    // 4. Return FromPropertyDescriptor(desc).
    return from_property_descriptor(vm, descriptor);
}

// 28.1.7 Reflect.getPrototypeOf ( target ), https://tc39.es/ecma262/#sec-reflect.getprototypeof
JS_DEFINE_NATIVE_FUNCTION(ReflectObject::get_prototype_of)
{
    auto target = vm.argument(0);

    // 1. If Type(target) is not Object, throw a TypeError exception.
    if (!target.is_object())
        return vm.throw_completion<TypeError>(ErrorType::NotAnObject, target.to_string_without_side_effects());

    // 2. Return ? target.[[GetPrototypeOf]]().
    return TRY(target.as_object().internal_get_prototype_of());
}

// 28.1.8 Reflect.has ( target, propertyKey ), https://tc39.es/ecma262/#sec-reflect.has
JS_DEFINE_NATIVE_FUNCTION(ReflectObject::has)
{
    auto target = vm.argument(0);
    auto property_key = vm.argument(1);

    // 1. If Type(target) is not Object, throw a TypeError exception.
    if (!target.is_object())
        return vm.throw_completion<TypeError>(ErrorType::NotAnObject, target.to_string_without_side_effects());

    // 2. Let key be ? ToPropertyKey(propertyKey).
    auto key = TRY(property_key.to_property_key(vm));

    // 3. Return ? target.[[HasProperty]](key).
    return Value(TRY(target.as_object().internal_has_property(key)));
}

// 28.1.9 Reflect.isExtensible ( target ), https://tc39.es/ecma262/#sec-reflect.isextensible
JS_DEFINE_NATIVE_FUNCTION(ReflectObject::is_extensible)
{
    auto target = vm.argument(0);

    // 1. If Type(target) is not Object, throw a TypeError exception.
    if (!target.is_object())
        return vm.throw_completion<TypeError>(ErrorType::NotAnObject, target.to_string_without_side_effects());

    // 2. Return ? target.[[IsExtensible]]().
    return Value(TRY(target.as_object().internal_is_extensible()));
}

// 28.1.10 Reflect.ownKeys ( target ), https://tc39.es/ecma262/#sec-reflect.ownkeys
JS_DEFINE_NATIVE_FUNCTION(ReflectObject::own_keys)
{
    auto& realm = *vm.current_realm();

    auto target = vm.argument(0);

    // 1. If Type(target) is not Object, throw a TypeError exception.
    if (!target.is_object())
        return vm.throw_completion<TypeError>(ErrorType::NotAnObject, target.to_string_without_side_effects());

    // 2. Let keys be ? target.[[OwnPropertyKeys]]().
    auto keys = TRY(target.as_object().internal_own_property_keys());

    // 3. Return CreateArrayFromList(keys).
    return Array::create_from(realm, keys);
}

// 28.1.11 Reflect.preventExtensions ( target ), https://tc39.es/ecma262/#sec-reflect.preventextensions
JS_DEFINE_NATIVE_FUNCTION(ReflectObject::prevent_extensions)
{
    auto target = vm.argument(0);

    // 1. If Type(target) is not Object, throw a TypeError exception.
    if (!target.is_object())
        return vm.throw_completion<TypeError>(ErrorType::NotAnObject, target.to_string_without_side_effects());

    // 2. Return ? target.[[PreventExtensions]]().
    return Value(TRY(target.as_object().internal_prevent_extensions()));
}

// 28.1.12 Reflect.set ( target, propertyKey, V [ , receiver ] ), https://tc39.es/ecma262/#sec-reflect.set
JS_DEFINE_NATIVE_FUNCTION(ReflectObject::set)
{
    auto target = vm.argument(0);
    auto property_key = vm.argument(1);
    auto value = vm.argument(2);
    auto receiver = vm.argument(3);

    // 1. If Type(target) is not Object, throw a TypeError exception.
    if (!target.is_object())
        return vm.throw_completion<TypeError>(ErrorType::NotAnObject, target.to_string_without_side_effects());

    // 2. Let key be ? ToPropertyKey(propertyKey).
    auto key = TRY(property_key.to_property_key(vm));

    // 3. If receiver is not present, then
    if (vm.argument_count() < 4) {
        // a. Set receiver to target.
        receiver = target;
    }

    // 4. Return ? target.[[Set]](key, V, receiver).
    return Value(TRY(target.as_object().internal_set(key, value, receiver)));
}

// 28.1.13 Reflect.setPrototypeOf ( target, proto ), https://tc39.es/ecma262/#sec-reflect.setprototypeof
JS_DEFINE_NATIVE_FUNCTION(ReflectObject::set_prototype_of)
{
    auto target = vm.argument(0);
    auto proto = vm.argument(1);

    // 1. If Type(target) is not Object, throw a TypeError exception.
    if (!target.is_object())
        return vm.throw_completion<TypeError>(ErrorType::NotAnObject, target.to_string_without_side_effects());

    // 2. If Type(proto) is not Object and proto is not null, throw a TypeError exception.
    if (!proto.is_object() && !proto.is_null())
        return vm.throw_completion<TypeError>(ErrorType::ObjectPrototypeWrongType);

    // 3. Return ? target.[[SetPrototypeOf]](proto).
    return Value(TRY(target.as_object().internal_set_prototype_of(proto.is_null() ? nullptr : &proto.as_object())));
}

}
