/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2020-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Function.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibJS/Runtime/ObjectConstructor.h>
#include <LibJS/Runtime/ProxyObject.h>
#include <LibJS/Runtime/Shape.h>

namespace JS {

JS_DEFINE_ALLOCATOR(ObjectConstructor);

ObjectConstructor::ObjectConstructor(Realm& realm)
    : NativeFunction(realm.vm().names.Object.as_string(), realm.intrinsics().function_prototype())
{
}

void ObjectConstructor::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);

    // 20.1.2.21 Object.prototype, https://tc39.es/ecma262/#sec-object.prototype
    define_direct_property(vm.names.prototype, realm.intrinsics().object_prototype(), 0);

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.defineProperty, define_property, 3, attr);
    define_native_function(realm, vm.names.defineProperties, define_properties, 2, attr);
    define_native_function(realm, vm.names.is, is, 2, attr);
    define_native_function(realm, vm.names.getOwnPropertyDescriptor, get_own_property_descriptor, 2, attr);
    define_native_function(realm, vm.names.getOwnPropertyDescriptors, get_own_property_descriptors, 1, attr);
    define_native_function(realm, vm.names.getOwnPropertyNames, get_own_property_names, 1, attr);
    define_native_function(realm, vm.names.getOwnPropertySymbols, get_own_property_symbols, 1, attr);
    define_native_function(realm, vm.names.getPrototypeOf, get_prototype_of, 1, attr);
    define_native_function(realm, vm.names.groupBy, group_by, 2, attr);
    define_native_function(realm, vm.names.setPrototypeOf, set_prototype_of, 2, attr);
    define_native_function(realm, vm.names.isExtensible, is_extensible, 1, attr);
    define_native_function(realm, vm.names.isFrozen, is_frozen, 1, attr);
    define_native_function(realm, vm.names.isSealed, is_sealed, 1, attr);
    define_native_function(realm, vm.names.preventExtensions, prevent_extensions, 1, attr);
    define_native_function(realm, vm.names.freeze, freeze, 1, attr);
    define_native_function(realm, vm.names.fromEntries, from_entries, 1, attr);
    define_native_function(realm, vm.names.seal, seal, 1, attr);
    define_native_function(realm, vm.names.keys, keys, 1, attr);
    define_native_function(realm, vm.names.values, values, 1, attr);
    define_native_function(realm, vm.names.entries, entries, 1, attr);
    define_native_function(realm, vm.names.create, create, 2, attr);
    define_native_function(realm, vm.names.hasOwn, has_own, 2, attr);
    define_native_function(realm, vm.names.assign, assign, 2, attr);

    define_direct_property(vm.names.length, Value(1), Attribute::Configurable);
}

// 20.1.1.1 Object ( [ value ] ), https://tc39.es/ecma262/#sec-object-value
ThrowCompletionOr<Value> ObjectConstructor::call()
{
    return TRY(construct(*this));
}

// 20.1.1.1 Object ( [ value ] ), https://tc39.es/ecma262/#sec-object-value
ThrowCompletionOr<NonnullGCPtr<Object>> ObjectConstructor::construct(FunctionObject& new_target)
{
    auto& vm = this->vm();
    auto& realm = *vm.current_realm();
    auto value = vm.argument(0);

    // 1. If NewTarget is neither undefined nor the active function object, then
    if (&new_target != this) {
        // a. Return ? OrdinaryCreateFromConstructor(NewTarget, "%Object.prototype%").
        return TRY(ordinary_create_from_constructor<Object>(vm, new_target, &Intrinsics::object_prototype, ConstructWithPrototypeTag::Tag));
    }

    // 2. If value is either undefined or null, return OrdinaryObjectCreate(%Object.prototype%).
    if (value.is_nullish())
        return Object::create(realm, realm.intrinsics().object_prototype());

    // 3. Return ! ToObject(value).
    return MUST(value.to_object(vm));
}

enum class GetOwnPropertyKeysType {
    String,
    Symbol,
};

// 20.1.2.11.1 GetOwnPropertyKeys ( O, type ), https://tc39.es/ecma262/#sec-getownpropertykeys
static ThrowCompletionOr<MarkedVector<Value>> get_own_property_keys(VM& vm, Value value, GetOwnPropertyKeysType type)
{
    // 1. Let obj be ? ToObject(O).
    auto object = TRY(value.to_object(vm));

    // 2. Let keys be ? obj.[[OwnPropertyKeys]]().
    auto keys = TRY(object->internal_own_property_keys());

    // 3. Let nameList be a new empty List.
    auto name_list = MarkedVector<Value> { vm.heap() };

    // 4. For each element nextKey of keys, do
    for (auto& next_key : keys) {
        // a. If Type(nextKey) is Symbol and type is symbol or Type(nextKey) is String and type is string, then
        if ((next_key.is_symbol() && type == GetOwnPropertyKeysType::Symbol) || (next_key.is_string() && type == GetOwnPropertyKeysType::String)) {
            // i. Append nextKey as the last element of nameList.
            name_list.append(next_key);
        }
    }

    // 5. Return nameList.
    return { move(name_list) };
}

// 20.1.2.1 Object.assign ( target, ...sources ), https://tc39.es/ecma262/#sec-object.assign
JS_DEFINE_NATIVE_FUNCTION(ObjectConstructor::assign)
{
    // 1. Let to be ? ToObject(target).
    auto to = TRY(vm.argument(0).to_object(vm));

    // 2. If only one argument was passed, return to.
    if (vm.argument_count() == 1)
        return to;

    // 3. For each element nextSource of sources, do
    for (size_t i = 1; i < vm.argument_count(); ++i) {
        auto next_source = vm.argument(i);

        // a. If nextSource is neither undefined nor null, then
        if (next_source.is_nullish())
            continue;

        // i. Let from be ! ToObject(nextSource).
        auto from = MUST(next_source.to_object(vm));

        // ii. Let keys be ? from.[[OwnPropertyKeys]]().
        auto keys = TRY(from->internal_own_property_keys());

        // iii. For each element nextKey of keys, do
        for (auto& next_key : keys) {
            auto property_key = MUST(PropertyKey::from_value(vm, next_key));

            // 1. Let desc be ? from.[[GetOwnProperty]](nextKey).
            auto desc = TRY(from->internal_get_own_property(property_key));

            // 2. If desc is not undefined and desc.[[Enumerable]] is true, then
            if (!desc.has_value() || !*desc->enumerable)
                continue;

            // a. Let propValue be ? Get(from, nextKey).
            auto prop_value = TRY(from->get(property_key));

            // b. Perform ? Set(to, nextKey, propValue, true).
            TRY(to->set(property_key, prop_value, Object::ShouldThrowExceptions::Yes));
        }
    }

    // 4. Return to.
    return to;
}

// 20.1.2.2 Object.create ( O, Properties ), https://tc39.es/ecma262/#sec-object.create
JS_DEFINE_NATIVE_FUNCTION(ObjectConstructor::create)
{
    auto& realm = *vm.current_realm();

    auto proto = vm.argument(0);
    auto properties = vm.argument(1);

    // 1. If Type(O) is neither Object nor Null, throw a TypeError exception.
    if (!proto.is_object() && !proto.is_null())
        return vm.throw_completion<TypeError>(ErrorType::ObjectPrototypeWrongType);

    // 2. Let obj be OrdinaryObjectCreate(O).
    auto object = Object::create(realm, proto.is_null() ? nullptr : &proto.as_object());

    // 3. If Properties is not undefined, then
    if (!properties.is_undefined()) {
        // a. Return ? ObjectDefineProperties(obj, Properties).
        return TRY(object->define_properties(properties));
    }

    // 4. Return obj.
    return object;
}

// 20.1.2.3 Object.defineProperties ( O, Properties ), https://tc39.es/ecma262/#sec-object.defineproperties
JS_DEFINE_NATIVE_FUNCTION(ObjectConstructor::define_properties)
{
    auto object = vm.argument(0);
    auto properties = vm.argument(1);

    // 1. If Type(O) is not Object, throw a TypeError exception.
    if (!object.is_object())
        return vm.throw_completion<TypeError>(ErrorType::NotAnObject, "Object argument");

    // 2. Return ? ObjectDefineProperties(O, Properties).
    return TRY(object.as_object().define_properties(properties));
}

// 20.1.2.4 Object.defineProperty ( O, P, Attributes ), https://tc39.es/ecma262/#sec-object.defineproperty
JS_DEFINE_NATIVE_FUNCTION(ObjectConstructor::define_property)
{
    // 1. If O is not an Object, throw a TypeError exception.
    if (!vm.argument(0).is_object())
        return vm.throw_completion<TypeError>(ErrorType::NotAnObject, vm.argument(0).to_string_without_side_effects());

    auto object = MUST(vm.argument(0).to_object(vm));

    // 2. Let key be ? ToPropertyKey(P).
    auto key = TRY(vm.argument(1).to_property_key(vm));

    // 3. Let desc be ? ToPropertyDescriptor(Attributes).
    auto descriptor = TRY(to_property_descriptor(vm, vm.argument(2)));

    // 4. Perform ? DefinePropertyOrThrow(O, key, desc).
    TRY(object->define_property_or_throw(key, descriptor));

    // 5. Return O.
    return object;
}

// 20.1.2.5 Object.entries ( O ), https://tc39.es/ecma262/#sec-object.entries
JS_DEFINE_NATIVE_FUNCTION(ObjectConstructor::entries)
{
    auto& realm = *vm.current_realm();

    // 1. Let obj be ? ToObject(O).
    auto object = TRY(vm.argument(0).to_object(vm));

    // 2. Let entryList be ? EnumerableOwnProperties(obj, key+value).
    auto name_list = TRY(object->enumerable_own_property_names(PropertyKind::KeyAndValue));

    // 3. Return CreateArrayFromList(entryList).
    return Array::create_from(realm, name_list);
}

// 20.1.2.6 Object.freeze ( O ), https://tc39.es/ecma262/#sec-object.freeze
JS_DEFINE_NATIVE_FUNCTION(ObjectConstructor::freeze)
{
    auto argument = vm.argument(0);

    // 1. If O is not an Object, return O.
    if (!argument.is_object())
        return argument;

    // 2. Let status be ? SetIntegrityLevel(O, frozen).
    auto status = TRY(argument.as_object().set_integrity_level(Object::IntegrityLevel::Frozen));

    // 3. If status is false, throw a TypeError exception.
    if (!status)
        return vm.throw_completion<TypeError>(ErrorType::ObjectFreezeFailed);

    // 4. Return O.
    return argument;
}

// 20.1.2.7 Object.fromEntries ( iterable ), https://tc39.es/ecma262/#sec-object.fromentries
JS_DEFINE_NATIVE_FUNCTION(ObjectConstructor::from_entries)
{
    auto& realm = *vm.current_realm();

    // 1. Perform ? RequireObjectCoercible(iterable).
    auto iterable = TRY(require_object_coercible(vm, vm.argument(0)));

    // 2. Let obj be OrdinaryObjectCreate(%Object.prototype%).
    auto object = Object::create(realm, realm.intrinsics().object_prototype());

    // 3. Assert: obj is an extensible ordinary object with no own properties.

    // 4. Let closure be a new Abstract Closure with parameters (key, value) that captures obj and performs the following steps when called:
    // 5. Let adder be CreateBuiltinFunction(closure, 2, "", « »).
    // 6. Return ? AddEntriesFromIterable(obj, iterable, adder).
    (void)TRY(get_iterator_values(vm, iterable, [&](Value iterator_value) -> Optional<Completion> {
        if (!iterator_value.is_object())
            return vm.throw_completion<TypeError>(ErrorType::NotAnObject, ByteString::formatted("Iterator value {}", iterator_value.to_string_without_side_effects()));

        auto key = TRY(iterator_value.as_object().get(0));
        auto value = TRY(iterator_value.as_object().get(1));

        // a. Let propertyKey be ? ToPropertyKey(key).
        auto property_key = TRY(key.to_property_key(vm));

        // b. Perform ! CreateDataPropertyOrThrow(obj, propertyKey, value).
        MUST(object->create_data_property_or_throw(property_key, value));

        // c. Return undefined.
        return {};
    }));

    return object;
}

// 20.1.2.8 Object.getOwnPropertyDescriptor ( O, P ), https://tc39.es/ecma262/#sec-object.getownpropertydescriptor
JS_DEFINE_NATIVE_FUNCTION(ObjectConstructor::get_own_property_descriptor)
{
    // 1. Let obj be ? ToObject(O).
    auto object = TRY(vm.argument(0).to_object(vm));

    // 2. Let key be ? ToPropertyKey(P).
    auto key = TRY(vm.argument(1).to_property_key(vm));

    // 3. Let desc be ? obj.[[GetOwnProperty]](key).
    auto descriptor = TRY(object->internal_get_own_property(key));

    // 4. Return FromPropertyDescriptor(desc).
    return from_property_descriptor(vm, descriptor);
}

// 20.1.2.9 Object.getOwnPropertyDescriptors ( O ), https://tc39.es/ecma262/#sec-object.getownpropertydescriptors
JS_DEFINE_NATIVE_FUNCTION(ObjectConstructor::get_own_property_descriptors)
{
    auto& realm = *vm.current_realm();

    // 1. Let obj be ? ToObject(O).
    auto object = TRY(vm.argument(0).to_object(vm));

    // 2. Let ownKeys be ? obj.[[OwnPropertyKeys]]().
    auto own_keys = TRY(object->internal_own_property_keys());

    // 3. Let descriptors be OrdinaryObjectCreate(%Object.prototype%).
    auto descriptors = Object::create(realm, realm.intrinsics().object_prototype());

    // 4. For each element key of ownKeys, do
    for (auto& key : own_keys) {
        auto property_key = MUST(PropertyKey::from_value(vm, key));

        // a. Let desc be ? obj.[[GetOwnProperty]](key).
        auto desc = TRY(object->internal_get_own_property(property_key));

        // b. Let descriptor be FromPropertyDescriptor(desc).
        auto descriptor = from_property_descriptor(vm, desc);

        // c. If descriptor is not undefined, perform ! CreateDataPropertyOrThrow(descriptors, key, descriptor).
        if (!descriptor.is_undefined())
            MUST(descriptors->create_data_property_or_throw(property_key, descriptor));
    }

    // 5. Return descriptors.
    return descriptors;
}

// 20.1.2.10 Object.getOwnPropertyNames ( O ), https://tc39.es/ecma262/#sec-object.getownpropertynames
JS_DEFINE_NATIVE_FUNCTION(ObjectConstructor::get_own_property_names)
{
    auto& realm = *vm.current_realm();

    // 1. Return CreateArrayFromList(? GetOwnPropertyKeys(O, string)).
    return Array::create_from(realm, TRY(get_own_property_keys(vm, vm.argument(0), GetOwnPropertyKeysType::String)));
}

// 20.1.2.11 Object.getOwnPropertySymbols ( O ), https://tc39.es/ecma262/#sec-object.getownpropertysymbols
JS_DEFINE_NATIVE_FUNCTION(ObjectConstructor::get_own_property_symbols)
{
    auto& realm = *vm.current_realm();

    // 1. Return CreateArrayFromList(? GetOwnPropertyKeys(O, symbol)).
    return Array::create_from(realm, TRY(get_own_property_keys(vm, vm.argument(0), GetOwnPropertyKeysType::Symbol)));
}

// 20.1.2.12 Object.getPrototypeOf ( O ), https://tc39.es/ecma262/#sec-object.getprototypeof
JS_DEFINE_NATIVE_FUNCTION(ObjectConstructor::get_prototype_of)
{
    // 1. Let obj be ? ToObject(O).
    auto object = TRY(vm.argument(0).to_object(vm));

    // 2. Return ? obj.[[GetPrototypeOf]]().
    return TRY(object->internal_get_prototype_of());
}

// 20.1.2.13 Object.groupBy ( items, callbackfn ), https://tc39.es/ecma262/#sec-object.groupby
JS_DEFINE_NATIVE_FUNCTION(ObjectConstructor::group_by)
{
    auto& realm = *vm.current_realm();

    auto items = vm.argument(0);
    auto callback_function = vm.argument(1);

    // 1. Let groups be ? GroupBy(items, callbackfn, property).
    auto groups = TRY((JS::group_by<OrderedHashMap<PropertyKey, MarkedVector<Value>>, PropertyKey>(vm, items, callback_function)));

    // 2. Let obj be OrdinaryObjectCreate(null).
    auto object = Object::create(realm, nullptr);

    // 3. For each Record { [[Key]], [[Elements]] } g of groups, do
    for (auto& group : groups) {
        // a. Let elements be CreateArrayFromList(g.[[Elements]]).
        auto elements = Array::create_from(realm, group.value);

        // b. Perform ! CreateDataPropertyOrThrow(obj, g.[[Key]], elements).
        MUST(object->create_data_property_or_throw(group.key, elements));
    }

    // 4. Return obj.
    return object;
}

// 20.1.2.14 Object.hasOwn ( O, P ), https://tc39.es/ecma262/#sec-object.hasown
JS_DEFINE_NATIVE_FUNCTION(ObjectConstructor::has_own)
{
    // 1. Let obj be ? ToObject(O).
    auto object = TRY(vm.argument(0).to_object(vm));

    // 2. Let key be ? ToPropertyKey(P).
    auto key = TRY(vm.argument(1).to_property_key(vm));

    // 3. Return ? HasOwnProperty(obj, key).
    return Value(TRY(object->has_own_property(key)));
}

// 20.1.2.15 Object.is ( value1, value2 ), https://tc39.es/ecma262/#sec-object.is
JS_DEFINE_NATIVE_FUNCTION(ObjectConstructor::is)
{
    // 1. Return SameValue(value1, value2).
    return Value(same_value(vm.argument(0), vm.argument(1)));
}

// 20.1.2.16 Object.isExtensible ( O ), https://tc39.es/ecma262/#sec-object.isextensible
JS_DEFINE_NATIVE_FUNCTION(ObjectConstructor::is_extensible)
{
    auto argument = vm.argument(0);

    // 1. If O is not an Object, return false.
    if (!argument.is_object())
        return Value(false);

    // 2. Return ? IsExtensible(O).
    return Value(TRY(argument.as_object().is_extensible()));
}

// 20.1.2.17 Object.isFrozen ( O ), https://tc39.es/ecma262/#sec-object.isfrozen
JS_DEFINE_NATIVE_FUNCTION(ObjectConstructor::is_frozen)
{
    auto argument = vm.argument(0);

    // 1. If O is not an Object, return true.
    if (!argument.is_object())
        return Value(true);

    // 2. Return ? TestIntegrityLevel(O, frozen).
    return Value(TRY(argument.as_object().test_integrity_level(Object::IntegrityLevel::Frozen)));
}

// 20.1.2.18 Object.isSealed ( O ), https://tc39.es/ecma262/#sec-object.issealed
JS_DEFINE_NATIVE_FUNCTION(ObjectConstructor::is_sealed)
{
    auto argument = vm.argument(0);

    // 1. If O is not an Object, return true.
    if (!argument.is_object())
        return Value(true);

    // 2. Return ? TestIntegrityLevel(O, sealed).
    return Value(TRY(argument.as_object().test_integrity_level(Object::IntegrityLevel::Sealed)));
}

// 20.1.2.19 Object.keys ( O ), https://tc39.es/ecma262/#sec-object.keys
JS_DEFINE_NATIVE_FUNCTION(ObjectConstructor::keys)
{
    auto& realm = *vm.current_realm();

    // 1. Let obj be ? ToObject(O).
    auto object = TRY(vm.argument(0).to_object(vm));

    // 2. Let keyList be ? EnumerableOwnProperties(obj, key).
    auto name_list = TRY(object->enumerable_own_property_names(PropertyKind::Key));

    // 3. Return CreateArrayFromList(keyList).
    return Array::create_from(realm, name_list);
}

// 20.1.2.20 Object.preventExtensions ( O ), https://tc39.es/ecma262/#sec-object.preventextensions
JS_DEFINE_NATIVE_FUNCTION(ObjectConstructor::prevent_extensions)
{
    auto argument = vm.argument(0);

    // 1. If O is not an Object, return O.
    if (!argument.is_object())
        return argument;

    // 2. Let status be ? O.[[PreventExtensions]]().
    auto status = TRY(argument.as_object().internal_prevent_extensions());

    // 3. If status is false, throw a TypeError exception.
    if (!status) {
        // FIXME: Improve/contextualize error message
        return vm.throw_completion<TypeError>(ErrorType::ObjectPreventExtensionsReturnedFalse);
    }

    // 4. Return O.
    return argument;
}

// 20.1.2.22 Object.seal ( O ), https://tc39.es/ecma262/#sec-object.seal
JS_DEFINE_NATIVE_FUNCTION(ObjectConstructor::seal)
{
    auto argument = vm.argument(0);

    // 1. If O is not an Object, return O.
    if (!argument.is_object())
        return argument;

    // 2. Let status be ? SetIntegrityLevel(O, sealed).
    auto status = TRY(argument.as_object().set_integrity_level(Object::IntegrityLevel::Sealed));

    // 3. If status is false, throw a TypeError exception.
    if (!status)
        return vm.throw_completion<TypeError>(ErrorType::ObjectSealFailed);

    // 4. Return O.
    return argument;
}

// 20.1.2.23 Object.setPrototypeOf ( O, proto ), https://tc39.es/ecma262/#sec-object.setprototypeof
JS_DEFINE_NATIVE_FUNCTION(ObjectConstructor::set_prototype_of)
{
    auto proto = vm.argument(1);

    // 1. Set O to ? RequireObjectCoercible(O).
    auto object = TRY(require_object_coercible(vm, vm.argument(0)));

    // 2. If Type(proto) is neither Object nor Null, throw a TypeError exception.
    if (!proto.is_object() && !proto.is_null())
        return vm.throw_completion<TypeError>(ErrorType::ObjectPrototypeWrongType);

    // 3. If Type(O) is not Object, return O.
    if (!object.is_object())
        return object;

    // 4. Let status be ? O.[[SetPrototypeOf]](proto).
    auto status = TRY(object.as_object().internal_set_prototype_of(proto.is_null() ? nullptr : &proto.as_object()));

    // 5. If status is false, throw a TypeError exception.
    if (!status) {
        // FIXME: Improve/contextualize error message
        return vm.throw_completion<TypeError>(ErrorType::ObjectSetPrototypeOfReturnedFalse);
    }

    // 6. Return O.
    return object;
}

// 20.1.2.24 Object.values ( O ), https://tc39.es/ecma262/#sec-object.values
JS_DEFINE_NATIVE_FUNCTION(ObjectConstructor::values)
{
    auto& realm = *vm.current_realm();

    // 1. Let obj be ? ToObject(O).
    auto object = TRY(vm.argument(0).to_object(vm));

    // 2. Let valueList be ? EnumerableOwnProperties(obj, value).
    auto name_list = TRY(object->enumerable_own_property_names(PropertyKind::Value));

    // 3. Return CreateArrayFromList(valueList).
    return Array::create_from(realm, name_list);
}

}
