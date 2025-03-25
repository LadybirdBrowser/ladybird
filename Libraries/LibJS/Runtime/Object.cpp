/*
 * Copyright (c) 2020-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2020-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/TypeCasts.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Accessor.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/ClassFieldDefinition.h>
#include <LibJS/Runtime/ECMAScriptFunctionObject.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/PropertyDescriptor.h>
#include <LibJS/Runtime/ProxyObject.h>
#include <LibJS/Runtime/Shape.h>
#include <LibJS/Runtime/Value.h>

namespace JS {

GC_DEFINE_ALLOCATOR(Object);

static HashMap<GC::Ptr<Object const>, HashMap<FlyString, Object::IntrinsicAccessor>> s_intrinsics;

// 10.1.12 OrdinaryObjectCreate ( proto [ , additionalInternalSlotsList ] ), https://tc39.es/ecma262/#sec-ordinaryobjectcreate
GC::Ref<Object> Object::create(Realm& realm, Object* prototype)
{
    if (!prototype)
        return realm.create<Object>(realm.intrinsics().empty_object_shape());
    if (prototype == realm.intrinsics().object_prototype())
        return realm.create<Object>(realm.intrinsics().new_object_shape());
    return realm.create<Object>(ConstructWithPrototypeTag::Tag, *prototype);
}

GC::Ref<Object> Object::create_prototype(Realm& realm, Object* prototype)
{
    auto shape = realm.heap().allocate<Shape>(realm);
    if (prototype)
        shape->set_prototype_without_transition(prototype);
    return realm.create<Object>(shape);
}

GC::Ref<Object> Object::create_with_premade_shape(Shape& shape)
{
    return shape.realm().create<Object>(shape);
}

Object::Object(GlobalObjectTag, Realm& realm, MayInterfereWithIndexedPropertyAccess may_interfere_with_indexed_property_access)
    : m_may_interfere_with_indexed_property_access(may_interfere_with_indexed_property_access == MayInterfereWithIndexedPropertyAccess::Yes)
{
    // This is the global object
    m_shape = heap().allocate<Shape>(realm);
}

Object::Object(ConstructWithoutPrototypeTag, Realm& realm, MayInterfereWithIndexedPropertyAccess may_interfere_with_indexed_property_access)
    : m_may_interfere_with_indexed_property_access(may_interfere_with_indexed_property_access == MayInterfereWithIndexedPropertyAccess::Yes)
{
    m_shape = heap().allocate<Shape>(realm);
}

Object::Object(Realm& realm, Object* prototype, MayInterfereWithIndexedPropertyAccess may_interfere_with_indexed_property_access)
    : m_may_interfere_with_indexed_property_access(may_interfere_with_indexed_property_access == MayInterfereWithIndexedPropertyAccess::Yes)
{
    m_shape = realm.intrinsics().empty_object_shape();
    VERIFY(m_shape);
    if (prototype != nullptr)
        set_prototype(prototype);
}

Object::Object(ConstructWithPrototypeTag, Object& prototype, MayInterfereWithIndexedPropertyAccess may_interfere_with_indexed_property_access)
    : m_may_interfere_with_indexed_property_access(may_interfere_with_indexed_property_access == MayInterfereWithIndexedPropertyAccess::Yes)
{
    m_shape = prototype.shape().realm().intrinsics().empty_object_shape();
    VERIFY(m_shape);
    set_prototype(&prototype);
}

Object::Object(Shape& shape, MayInterfereWithIndexedPropertyAccess may_interfere_with_indexed_property_access)
    : m_may_interfere_with_indexed_property_access(may_interfere_with_indexed_property_access == MayInterfereWithIndexedPropertyAccess::Yes)
    , m_shape(&shape)
{
    m_storage.resize(shape.property_count());
}

Object::~Object()
{
    if (m_has_intrinsic_accessors)
        s_intrinsics.remove(this);
}

void Object::initialize(Realm&)
{
}

void Object::unsafe_set_shape(Shape& shape)
{
    m_shape = shape;
    m_storage.resize(shape.property_count());
}

// 7.2 Testing and Comparison Operations, https://tc39.es/ecma262/#sec-testing-and-comparison-operations

// 7.2.5 IsExtensible ( O ), https://tc39.es/ecma262/#sec-isextensible-o
ThrowCompletionOr<bool> Object::is_extensible() const
{
    // 1. Return ? O.[[IsExtensible]]().
    return internal_is_extensible();
}

// 7.3 Operations on Objects, https://tc39.es/ecma262/#sec-operations-on-objects

// 7.3.2 Get ( O, P ), https://tc39.es/ecma262/#sec-get-o-p
ThrowCompletionOr<Value> Object::get(PropertyKey const& property_key) const
{
    // 1. Return ? O.[[Get]](P, O).
    return TRY(internal_get(property_key, this));
}

// NOTE: 7.3.3 GetV ( V, P ) is implemented as Value::get().

// 7.3.4 Set ( O, P, V, Throw ), https://tc39.es/ecma262/#sec-set-o-p-v-throw
ThrowCompletionOr<void> Object::set(PropertyKey const& property_key, Value value, ShouldThrowExceptions throw_exceptions)
{
    auto& vm = this->vm();

    VERIFY(!value.is_empty());

    // 1. Let success be ? O.[[Set]](P, V, O).
    auto success = TRY(internal_set(property_key, value, this));

    // 2. If success is false and Throw is true, throw a TypeError exception.
    if (!success && throw_exceptions == ShouldThrowExceptions::Yes) {
        // FIXME: Improve/contextualize error message
        return vm.throw_completion<TypeError>(ErrorType::ObjectSetReturnedFalse);
    }

    // 3. Return unused.
    return {};
}

// 7.3.5 CreateDataProperty ( O, P, V ), https://tc39.es/ecma262/#sec-createdataproperty
ThrowCompletionOr<bool> Object::create_data_property(PropertyKey const& property_key, Value value)
{
    // 1. Let newDesc be the PropertyDescriptor { [[Value]]: V, [[Writable]]: true, [[Enumerable]]: true, [[Configurable]]: true }.
    auto new_descriptor = PropertyDescriptor {
        .value = value,
        .writable = true,
        .enumerable = true,
        .configurable = true,
    };

    // 2. Return ? O.[[DefineOwnProperty]](P, newDesc).
    return internal_define_own_property(property_key, new_descriptor);
}

// 7.3.6 CreateMethodProperty ( O, P, V ), https://tc39.es/ecma262/#sec-createmethodproperty
void Object::create_method_property(PropertyKey const& property_key, Value value)
{
    VERIFY(!value.is_empty());

    // 1. Assert: O is an ordinary, extensible object with no non-configurable properties.

    // 2. Let newDesc be the PropertyDescriptor { [[Value]]: V, [[Writable]]: true, [[Enumerable]]: false, [[Configurable]]: true }.
    auto new_descriptor = PropertyDescriptor {
        .value = value,
        .writable = true,
        .enumerable = false,
        .configurable = true,
    };

    // 3. Perform ! O.[[DefineOwnProperty]](P, newDesc).
    MUST(internal_define_own_property(property_key, new_descriptor));

    // 4. Return unused.
}

// 7.3.7 CreateDataPropertyOrThrow ( O, P, V ), https://tc39.es/ecma262/#sec-createdatapropertyorthrow
ThrowCompletionOr<bool> Object::create_data_property_or_throw(PropertyKey const& property_key, Value value)
{
    auto& vm = this->vm();

    VERIFY(!value.is_empty());

    // 1. Let success be ? CreateDataProperty(O, P, V).
    auto success = TRY(create_data_property(property_key, value));

    // 2. If success is false, throw a TypeError exception.
    if (!success) {
        // FIXME: Improve/contextualize error message
        return vm.throw_completion<TypeError>(ErrorType::ObjectDefineOwnPropertyReturnedFalse);
    }

    // 3. Return success.
    return success;
}

// 7.3.8 CreateNonEnumerableDataPropertyOrThrow ( O, P, V ), https://tc39.es/ecma262/#sec-createnonenumerabledatapropertyorthrow
void Object::create_non_enumerable_data_property_or_throw(PropertyKey const& property_key, Value value)
{
    VERIFY(!value.is_empty());

    // 1. Assert: O is an ordinary, extensible object with no non-configurable properties.

    // 2. Let newDesc be the PropertyDescriptor { [[Value]]: V, [[Writable]]: true, [[Enumerable]]: false, [[Configurable]]: true }.
    auto new_description = PropertyDescriptor { .value = value, .writable = true, .enumerable = false, .configurable = true };

    // 3. Perform ! DefinePropertyOrThrow(O, P, newDesc).
    MUST(define_property_or_throw(property_key, new_description));

    // 4. Return unused.
}

// 7.3.9 DefinePropertyOrThrow ( O, P, desc ), https://tc39.es/ecma262/#sec-definepropertyorthrow
ThrowCompletionOr<void> Object::define_property_or_throw(PropertyKey const& property_key, PropertyDescriptor const& property_descriptor)
{
    auto& vm = this->vm();

    // 1. Let success be ? O.[[DefineOwnProperty]](P, desc).
    auto success = TRY(internal_define_own_property(property_key, property_descriptor));

    // 2. If success is false, throw a TypeError exception.
    if (!success) {
        // FIXME: Improve/contextualize error message
        return vm.throw_completion<TypeError>(ErrorType::ObjectDefineOwnPropertyReturnedFalse);
    }

    // 3. Return unused.
    return {};
}

// 7.3.10 DeletePropertyOrThrow ( O, P ), https://tc39.es/ecma262/#sec-deletepropertyorthrow
ThrowCompletionOr<void> Object::delete_property_or_throw(PropertyKey const& property_key)
{
    auto& vm = this->vm();

    // 1. Let success be ? O.[[Delete]](P).
    auto success = TRY(internal_delete(property_key));

    // 2. If success is false, throw a TypeError exception.
    if (!success) {
        // FIXME: Improve/contextualize error message
        return vm.throw_completion<TypeError>(ErrorType::ObjectDeleteReturnedFalse);
    }

    // 3. Return unused.
    return {};
}

// 7.3.12 HasProperty ( O, P ), https://tc39.es/ecma262/#sec-hasproperty
ThrowCompletionOr<bool> Object::has_property(PropertyKey const& property_key) const
{
    // 1. Return ? O.[[HasProperty]](P).
    return internal_has_property(property_key);
}

// 7.3.13 HasOwnProperty ( O, P ), https://tc39.es/ecma262/#sec-hasownproperty
ThrowCompletionOr<bool> Object::has_own_property(PropertyKey const& property_key) const
{
    // 1. Let desc be ? O.[[GetOwnProperty]](P).
    auto descriptor = TRY(internal_get_own_property(property_key));

    // 2. If desc is undefined, return false.
    if (!descriptor.has_value())
        return false;

    // 3. Return true.
    return true;
}

// 7.3.16 SetIntegrityLevel ( O, level ), https://tc39.es/ecma262/#sec-setintegritylevel
ThrowCompletionOr<bool> Object::set_integrity_level(IntegrityLevel level)
{
    auto& vm = this->vm();

    // 1. Let status be ? O.[[PreventExtensions]]().
    auto status = TRY(internal_prevent_extensions());

    // 2. If status is false, return false.
    if (!status)
        return false;

    // 3. Let keys be ? O.[[OwnPropertyKeys]]().
    auto keys = TRY(internal_own_property_keys());

    // 4. If level is sealed, then
    if (level == IntegrityLevel::Sealed) {
        // a. For each element k of keys, do
        for (auto& key : keys) {
            auto property_key = MUST(PropertyKey::from_value(vm, key));

            // i. Perform ? DefinePropertyOrThrow(O, k, PropertyDescriptor { [[Configurable]]: false }).
            TRY(define_property_or_throw(property_key, { .configurable = false }));
        }
    }
    // 5. Else,
    else {
        // a. Assert: level is frozen.

        // b. For each element k of keys, do
        for (auto& key : keys) {
            auto property_key = MUST(PropertyKey::from_value(vm, key));

            // i. Let currentDesc be ? O.[[GetOwnProperty]](k).
            auto current_descriptor = TRY(internal_get_own_property(property_key));

            // ii. If currentDesc is not undefined, then
            if (!current_descriptor.has_value())
                continue;

            PropertyDescriptor descriptor;

            // 1. If IsAccessorDescriptor(currentDesc) is true, then
            if (current_descriptor->is_accessor_descriptor()) {
                // a. Let desc be the PropertyDescriptor { [[Configurable]]: false }.
                descriptor = { .configurable = false };
            }
            // 2. Else,
            else {
                // a. Let desc be the PropertyDescriptor { [[Configurable]]: false, [[Writable]]: false }.
                descriptor = { .writable = false, .configurable = false };
            }

            // 3. Perform ? DefinePropertyOrThrow(O, k, desc).
            TRY(define_property_or_throw(property_key, descriptor));
        }
    }

    // 6. Return true.
    return true;
}

// 7.3.17 TestIntegrityLevel ( O, level ), https://tc39.es/ecma262/#sec-testintegritylevel
ThrowCompletionOr<bool> Object::test_integrity_level(IntegrityLevel level) const
{
    auto& vm = this->vm();

    // 1. Let extensible be ? IsExtensible(O).
    auto extensible = TRY(is_extensible());

    // 2. If extensible is true, return false.
    // 3. NOTE: If the object is extensible, none of its properties are examined.
    if (extensible)
        return false;

    // 4. Let keys be ? O.[[OwnPropertyKeys]]().
    auto keys = TRY(internal_own_property_keys());

    // 5. For each element k of keys, do
    for (auto& key : keys) {
        auto property_key = MUST(PropertyKey::from_value(vm, key));

        // a. Let currentDesc be ? O.[[GetOwnProperty]](k).
        auto current_descriptor = TRY(internal_get_own_property(property_key));

        // b. If currentDesc is not undefined, then
        if (!current_descriptor.has_value())
            continue;
        // i. If currentDesc.[[Configurable]] is true, return false.
        if (*current_descriptor->configurable)
            return false;

        // ii. If level is frozen and IsDataDescriptor(currentDesc) is true, then
        if (level == IntegrityLevel::Frozen && current_descriptor->is_data_descriptor()) {
            // 1. If currentDesc.[[Writable]] is true, return false.
            if (*current_descriptor->writable)
                return false;
        }
    }

    // 6. Return true.
    return true;
}

// 7.3.24 EnumerableOwnPropertyNames ( O, kind ), https://tc39.es/ecma262/#sec-enumerableownpropertynames
ThrowCompletionOr<GC::RootVector<Value>> Object::enumerable_own_property_names(PropertyKind kind) const
{
    // NOTE: This has been flattened for readability, so some `else` branches in the
    //       spec text have been replaced with `continue`s in the loop below.

    auto& vm = this->vm();
    auto& realm = *vm.current_realm();

    // 1. Let ownKeys be ? O.[[OwnPropertyKeys]]().
    auto own_keys = TRY(internal_own_property_keys());

    // 2. Let properties be a new empty List.
    auto properties = GC::RootVector<Value> { heap() };

    // 3. For each element key of ownKeys, do
    for (auto& key : own_keys) {
        // a. If Type(key) is String, then
        if (!key.is_string())
            continue;
        auto property_key = MUST(PropertyKey::from_value(vm, key));

        // i. Let desc be ? O.[[GetOwnProperty]](key).
        auto descriptor = TRY(internal_get_own_property(property_key));

        // ii. If desc is not undefined and desc.[[Enumerable]] is true, then
        if (descriptor.has_value() && *descriptor->enumerable) {
            // 1. If kind is key, append key to properties.
            if (kind == PropertyKind::Key) {
                properties.append(key);
                continue;
            }
            // 2. Else,

            // a. Let value be ? Get(O, key).
            auto value = TRY(get(property_key));

            // b. If kind is value, append value to properties.
            if (kind == PropertyKind::Value) {
                properties.append(value);
                continue;
            }
            // c. Else,

            // i. Assert: kind is key+value.
            VERIFY(kind == PropertyKind::KeyAndValue);

            // ii. Let entry be CreateArrayFromList(« key, value »).
            auto entry = Array::create_from(realm, { key, value });

            // iii. Append entry to properties.
            properties.append(entry);
        }
    }

    // 4. Return properties.
    return { move(properties) };
}

// 7.3.26 CopyDataProperties ( target, source, excludedItems ), https://tc39.es/ecma262/#sec-copydataproperties
// 14.6 CopyDataProperties ( target, source, excludedItems, excludedKeys [ , excludedValues ] ), https://tc39.es/proposal-temporal/#sec-copydataproperties
ThrowCompletionOr<void> Object::copy_data_properties(VM& vm, Value source, HashTable<PropertyKey> const& excluded_keys, HashTable<JS::Value> const& excluded_values)
{
    // 1. If source is either undefined or null, return unused.
    if (source.is_nullish())
        return {};

    // 2. Let from be ! ToObject(source).
    auto from = MUST(source.to_object(vm));

    // 3. Let keys be ? from.[[OwnPropertyKeys]]().
    auto keys = TRY(from->internal_own_property_keys());

    // 4. For each element nextKey of keys, do
    for (auto& next_key_value : keys) {
        auto next_key = MUST(PropertyKey::from_value(vm, next_key_value));

        // a. Let excluded be false.
        // b. For each element e of excludedKeys, do
        //    i. If SameValue(e, nextKey) is true, then
        //        1. Set excluded to true.
        if (excluded_keys.contains(next_key))
            continue;

        // c. If excluded is false, then

        // i. Let desc be ? from.[[GetOwnProperty]](nextKey).
        auto desc = TRY(from->internal_get_own_property(next_key));

        // ii. If desc is not undefined and desc.[[Enumerable]] is true, then
        if (desc.has_value() && desc->attributes().is_enumerable()) {
            // 1. Let propValue be ? Get(from, nextKey).
            auto prop_value = TRY(from->get(next_key));

            // 2. If excludedValues is present, then
            //     a. For each element e of excludedValues, do
            //         i. If SameValue(e, propValue) is true, then
            //             i. Set excluded to true.
            // 3. If excluded is false, Perform ! CreateDataPropertyOrThrow(target, nextKey, propValue).
            // NOTE: HashTable traits for JS::Value uses SameValue.
            if (!excluded_values.contains(prop_value))
                MUST(create_data_property_or_throw(next_key, prop_value));
        }
    }

    // 5. Return unused.
    return {};
}

// 14.7 SnapshotOwnProperties ( source, proto [ , excludedKeys [ , excludedValues ] ] ), https://tc39.es/proposal-temporal/#sec-snapshotownproperties
ThrowCompletionOr<GC::Ref<Object>> Object::snapshot_own_properties(VM& vm, GC::Ptr<Object> prototype, HashTable<PropertyKey> const& excluded_keys, HashTable<Value> const& excluded_values)
{
    auto& realm = *vm.current_realm();

    // 1. Let copy be OrdinaryObjectCreate(proto).
    auto copy = Object::create(realm, prototype);

    // 2. If excludedKeys is not present, set excludedKeys to « ».
    // 3. If excludedValues is not present, set excludedValues to « ».
    // 4. Perform ? CopyDataProperties(copy, source, excludedKeys, excludedValues).
    TRY(copy->copy_data_properties(vm, Value { this }, excluded_keys, excluded_values));

    // 5. Return copy.
    return copy;
}

// 7.3.27 PrivateElementFind ( O, P ), https://tc39.es/ecma262/#sec-privateelementfind
PrivateElement* Object::private_element_find(PrivateName const& name)
{
    if (!m_private_elements)
        return nullptr;

    // 1. If O.[[PrivateElements]] contains a PrivateElement pe such that pe.[[Key]] is P, then
    auto it = m_private_elements->find_if([&](auto const& element) {
        return element.key == name;
    });

    if (!it.is_end()) {
        // a. Return pe.
        return &(*it);
    }

    // 2. Return empty.
    return nullptr;
}

// 7.3.28 PrivateFieldAdd ( O, P, value ), https://tc39.es/ecma262/#sec-privatefieldadd
ThrowCompletionOr<void> Object::private_field_add(PrivateName const& name, Value value)
{
    auto& vm = this->vm();

    // 1. If the host is a web browser, then
    //    a. Perform ? HostEnsureCanAddPrivateElement(O).
    // NOTE: Since LibJS has no way of knowing whether it is in a browser we just always call the hook.
    TRY(vm.host_ensure_can_add_private_element(*this));

    // 2. Let entry be PrivateElementFind(O, P).
    // 3. If entry is not empty, throw a TypeError exception.
    if (auto* entry = private_element_find(name); entry)
        return vm.throw_completion<TypeError>(ErrorType::PrivateFieldAlreadyDeclared, name.description);

    if (!m_private_elements)
        m_private_elements = make<Vector<PrivateElement>>();

    // 4. Append PrivateElement { [[Key]]: P, [[Kind]]: field, [[Value]]: value } to O.[[PrivateElements]].
    m_private_elements->empend(name, PrivateElement::Kind::Field, value);

    // 5. Return unused.
    return {};
}

// 7.3.29 PrivateMethodOrAccessorAdd ( O, method ), https://tc39.es/ecma262/#sec-privatemethodoraccessoradd
ThrowCompletionOr<void> Object::private_method_or_accessor_add(PrivateElement element)
{
    auto& vm = this->vm();

    // 1. Assert: method.[[Kind]] is either method or accessor.
    VERIFY(element.kind == PrivateElement::Kind::Method || element.kind == PrivateElement::Kind::Accessor);

    // 2. If the host is a web browser, then
    //    a. Perform ? HostEnsureCanAddPrivateElement(O).
    // NOTE: Since LibJS has no way of knowing whether it is in a browser we just always call the hook.
    TRY(vm.host_ensure_can_add_private_element(*this));

    // 3. Let entry be PrivateElementFind(O, method.[[Key]]).
    // 4. If entry is not empty, throw a TypeError exception.
    if (auto* entry = private_element_find(element.key); entry)
        return vm.throw_completion<TypeError>(ErrorType::PrivateFieldAlreadyDeclared, element.key.description);

    if (!m_private_elements)
        m_private_elements = make<Vector<PrivateElement>>();

    // 5. Append method to O.[[PrivateElements]].
    m_private_elements->append(move(element));

    // 6. Return unused.
    return {};
}

// 7.3.31 PrivateGet ( O, P ), https://tc39.es/ecma262/#sec-privateget
ThrowCompletionOr<Value> Object::private_get(PrivateName const& name)
{
    auto& vm = this->vm();

    // 1. Let entry be PrivateElementFind(O, P).
    auto* entry = private_element_find(name);

    // 2. If entry is empty, throw a TypeError exception.
    if (!entry)
        return vm.throw_completion<TypeError>(ErrorType::PrivateFieldDoesNotExistOnObject, name.description);

    auto& value = entry->value;

    // 3. If entry.[[Kind]] is either field or method, then
    if (entry->kind != PrivateElement::Kind::Accessor) {
        // a. Return entry.[[Value]].
        return value;
    }

    // Assert: entry.[[Kind]] is accessor.
    VERIFY(value.is_accessor());

    // 6. Let getter be entry.[[Get]].
    auto* getter = value.as_accessor().getter();

    // 5. If entry.[[Get]] is undefined, throw a TypeError exception.
    if (!getter)
        return vm.throw_completion<TypeError>(ErrorType::PrivateFieldGetAccessorWithoutGetter, name.description);

    // 7. Return ? Call(getter, O).
    return TRY(call(vm, *getter, this));
}

// 7.3.32 PrivateSet ( O, P, value ), https://tc39.es/ecma262/#sec-privateset
ThrowCompletionOr<void> Object::private_set(PrivateName const& name, Value value)
{
    auto& vm = this->vm();

    // 1. Let entry be PrivateElementFind(O, P).
    auto* entry = private_element_find(name);

    // 2. If entry is empty, throw a TypeError exception.
    if (!entry)
        return vm.throw_completion<TypeError>(ErrorType::PrivateFieldDoesNotExistOnObject, name.description);

    // 3. If entry.[[Kind]] is field, then
    if (entry->kind == PrivateElement::Kind::Field) {
        // a. Set entry.[[Value]] to value.
        entry->value = value;
        return {};
    }
    // 4. Else if entry.[[Kind]] is method, then
    else if (entry->kind == PrivateElement::Kind::Method) {
        // a. Throw a TypeError exception.
        return vm.throw_completion<TypeError>(ErrorType::PrivateFieldSetMethod, name.description);
    }

    // 5. Else,

    // a. Assert: entry.[[Kind]] is accessor.
    VERIFY(entry->kind == PrivateElement::Kind::Accessor);

    auto& accessor = entry->value;
    VERIFY(accessor.is_accessor());

    // c. Let setter be entry.[[Set]].
    auto* setter = accessor.as_accessor().setter();

    // b. If entry.[[Set]] is undefined, throw a TypeError exception.
    if (!setter)
        return vm.throw_completion<TypeError>(ErrorType::PrivateFieldSetAccessorWithoutSetter, name.description);

    // d. Perform ? Call(setter, O, « value »).
    TRY(call(vm, *setter, this, value));

    // 6. Return unused.
    return {};
}

// 7.3.33 DefineField ( receiver, fieldRecord ), https://tc39.es/ecma262/#sec-definefield
ThrowCompletionOr<void> Object::define_field(ClassFieldDefinition const& field)
{
    auto& vm = this->vm();

    // 1. Let fieldName be fieldRecord.[[Name]].
    auto const& field_name = field.name;

    // 2. Let initializer be fieldRecord.[[Initializer]].
    auto const& initializer = field.initializer;

    auto init_value = js_undefined();

    // 3. If initializer is not empty, then
    if (initializer) {
        // a. Let initValue be ? Call(initializer, receiver).
        init_value = TRY(call(vm, initializer, this));
    }
    // 4. Else, let initValue be undefined.

    // 5. If fieldName is a Private Name, then
    if (field_name.has<PrivateName>()) {
        // a. Perform ? PrivateFieldAdd(receiver, fieldName, initValue).
        TRY(private_field_add(field_name.get<PrivateName>(), init_value));
    }
    // 6. Else,
    else {
        // a. Assert: IsPropertyKey(fieldName) is true.
        // b. Perform ? CreateDataPropertyOrThrow(receiver, fieldName, initValue).
        TRY(create_data_property_or_throw(field_name.get<PropertyKey>(), init_value));
    }

    // 7. Return unused.
    return {};
}

// 7.3.34 InitializeInstanceElements ( O, constructor ), https://tc39.es/ecma262/#sec-initializeinstanceelements
ThrowCompletionOr<void> Object::initialize_instance_elements(ECMAScriptFunctionObject& constructor)
{
    // 1. Let methods be the value of constructor.[[PrivateMethods]].
    // 2. For each PrivateElement method of methods, do
    for (auto const& method : constructor.private_methods()) {
        // a. Perform ? PrivateMethodOrAccessorAdd(O, method).
        TRY(private_method_or_accessor_add(method));
    }

    // 3. Let fields be the value of constructor.[[Fields]].
    // 4. For each element fieldRecord of fields, do
    for (auto const& field : constructor.fields()) {
        // a. Perform ? DefineField(O, fieldRecord).
        TRY(define_field(field));
    }

    // 5. Return unused.
    return {};
}

// 10.1 Ordinary Object Internal Methods and Internal Slots, https://tc39.es/ecma262/#sec-ordinary-object-internal-methods-and-internal-slots

// 10.1.1 [[GetPrototypeOf]] ( ), https://tc39.es/ecma262/#sec-ordinary-object-internal-methods-and-internal-slots-getprototypeof
ThrowCompletionOr<Object*> Object::internal_get_prototype_of() const
{
    // 1. Return O.[[Prototype]].
    return const_cast<Object*>(prototype());
}

// 10.1.2 [[SetPrototypeOf]] ( V ), https://tc39.es/ecma262/#sec-ordinary-object-internal-methods-and-internal-slots-setprototypeof-v
ThrowCompletionOr<bool> Object::internal_set_prototype_of(Object* new_prototype)
{
    // 1. Let current be O.[[Prototype]].
    // 2. If SameValue(V, current) is true, return true.
    if (prototype() == new_prototype)
        return true;

    // 3. Let extensible be O.[[Extensible]].
    // 4. If extensible is false, return false.
    if (!m_is_extensible)
        return false;

    // 5. Let p be V.
    auto* prototype = new_prototype;

    // 6. Let done be false.
    // 7. Repeat, while done is false,
    while (prototype) {
        // a. If p is null, set done to true.

        // b. Else if SameValue(p, O) is true, return false.
        if (prototype == this)
            return false;
        // c. Else,

        // i. If p.[[GetPrototypeOf]] is not the ordinary object internal method defined in 10.1.1, set done to true.
        // NOTE: This is a best-effort implementation; we don't have a good way of detecting whether certain virtual
        // Object methods have been overridden by a given object, but as ProxyObject is the only one doing that for
        // [[SetPrototypeOf]], this check does the trick.
        if (is<ProxyObject>(prototype))
            break;

        // ii. Else, set p to p.[[Prototype]].
        prototype = prototype->prototype();
    }

    // 8. Set O.[[Prototype]] to V.
    set_prototype(new_prototype);

    // 9. Return true.
    return true;
}

// 10.1.3 [[IsExtensible]] ( ), https://tc39.es/ecma262/#sec-ordinary-object-internal-methods-and-internal-slots-isextensible
ThrowCompletionOr<bool> Object::internal_is_extensible() const
{
    // 1. Return O.[[Extensible]].
    return m_is_extensible;
}

// 10.1.4 [[PreventExtensions]] ( ), https://tc39.es/ecma262/#sec-ordinary-object-internal-methods-and-internal-slots-preventextensions
ThrowCompletionOr<bool> Object::internal_prevent_extensions()
{
    // 1. Set O.[[Extensible]] to false.
    m_is_extensible = false;

    // 2. Return true.
    return true;
}

// 10.1.5 [[GetOwnProperty]] ( P ), https://tc39.es/ecma262/#sec-ordinary-object-internal-methods-and-internal-slots-getownproperty-p
// 10.1.5.1 OrdinaryGetOwnProperty ( O, P ) https://tc39.es/ecma262/#sec-ordinarygetownproperty
ThrowCompletionOr<Optional<PropertyDescriptor>> Object::internal_get_own_property(PropertyKey const& property_key) const
{
    // 1. If O does not have an own property with key P, return undefined.
    auto maybe_storage_entry = storage_get(property_key);
    if (!maybe_storage_entry.has_value())
        return Optional<PropertyDescriptor> {};

    // 2. Let D be a newly created Property Descriptor with no fields.
    PropertyDescriptor descriptor;

    // 3. Let X be O's own property whose key is P.
    auto [value, attributes, property_offset] = *maybe_storage_entry;

    // AD-HOC: Properties with the [[Unimplemented]] attribute are used for reporting unimplemented IDL interfaces.
    if (attributes.is_unimplemented()) {
        if (vm().on_unimplemented_property_access)
            vm().on_unimplemented_property_access(*this, property_key);
        descriptor.unimplemented = true;
    }

    // 4. If X is a data property, then
    if (!value.is_accessor()) {
        // a. Set D.[[Value]] to the value of X's [[Value]] attribute.
        descriptor.value = value.value_or(js_undefined());

        // b. Set D.[[Writable]] to the value of X's [[Writable]] attribute.
        descriptor.writable = attributes.is_writable();
    }
    // 5. Else,
    else {
        // a. Assert: X is an accessor property.

        // b. Set D.[[Get]] to the value of X's [[Get]] attribute.
        descriptor.get = value.as_accessor().getter();

        // c. Set D.[[Set]] to the value of X's [[Set]] attribute.
        descriptor.set = value.as_accessor().setter();
    }

    // 6. Set D.[[Enumerable]] to the value of X's [[Enumerable]] attribute.
    descriptor.enumerable = attributes.is_enumerable();

    // 7. Set D.[[Configurable]] to the value of X's [[Configurable]] attribute.
    descriptor.configurable = attributes.is_configurable();

    // Non-standard: Add the property offset to the descriptor. This is used to populate CacheablePropertyMetadata.
    descriptor.property_offset = property_offset;

    // 8. Return D.
    return descriptor;
}

// 10.1.6 [[DefineOwnProperty]] ( P, Desc ), https://tc39.es/ecma262/#sec-ordinary-object-internal-methods-and-internal-slots-defineownproperty-p-desc
// 10.1.6.1 OrdinaryDefineOwnProperty ( O, P, Desc ), https://tc39.es/ecma262/#sec-ordinarydefineownproperty
ThrowCompletionOr<bool> Object::internal_define_own_property(PropertyKey const& property_key, PropertyDescriptor const& property_descriptor, Optional<PropertyDescriptor>* precomputed_get_own_property)
{
    // 1. Let current be ? O.[[GetOwnProperty]](P).
    auto current = precomputed_get_own_property ? *precomputed_get_own_property : TRY(internal_get_own_property(property_key));

    // 2. Let extensible be ? IsExtensible(O).
    auto extensible = TRY(is_extensible());

    // 3. Return ValidateAndApplyPropertyDescriptor(O, P, extensible, Desc, current).
    return validate_and_apply_property_descriptor(this, property_key, extensible, property_descriptor, current);
}

// 10.1.7 [[HasProperty]] ( P ), https://tc39.es/ecma262/#sec-ordinary-object-internal-methods-and-internal-slots-hasproperty-p
// 10.1.7.1 OrdinaryHasProperty ( O, P ), https://tc39.es/ecma262/#sec-ordinaryhasproperty
ThrowCompletionOr<bool> Object::internal_has_property(PropertyKey const& property_key) const
{
    // 1. Let hasOwn be ? O.[[GetOwnProperty]](P).
    auto has_own = TRY(internal_get_own_property(property_key));

    // 2. If hasOwn is not undefined, return true.
    if (has_own.has_value())
        return true;

    // 3. Let parent be ? O.[[GetPrototypeOf]]().
    auto* parent = TRY(internal_get_prototype_of());

    // 4. If parent is not null, then
    if (parent) {
        // a. Return ? parent.[[HasProperty]](P).
        return parent->internal_has_property(property_key);
    }

    // 5. Return false.
    return false;
}

// 10.1.8 [[Get]] ( P, Receiver ), https://tc39.es/ecma262/#sec-ordinary-object-internal-methods-and-internal-slots-get-p-receiver
// 10.1.8.1 OrdinaryGet ( O, P, Receiver ), https://tc39.es/ecma262/#sec-ordinaryget
ThrowCompletionOr<Value> Object::internal_get(PropertyKey const& property_key, Value receiver, CacheablePropertyMetadata* cacheable_metadata, PropertyLookupPhase phase) const
{
    VERIFY(!receiver.is_empty());

    auto& vm = this->vm();

    // 1. Let desc be ? O.[[GetOwnProperty]](P).
    auto descriptor = TRY(internal_get_own_property(property_key));

    // 2. If desc is undefined, then
    if (!descriptor.has_value()) {
        // a. Let parent be ? O.[[GetPrototypeOf]]().
        auto* parent = TRY(internal_get_prototype_of());

        // b. If parent is null, return undefined.
        if (!parent)
            return js_undefined();

        // c. Return ? parent.[[Get]](P, Receiver).
        return parent->internal_get(property_key, receiver, cacheable_metadata, PropertyLookupPhase::PrototypeChain);
    }

    auto update_inline_cache = [&] {
        // Non-standard: If the caller has requested cacheable metadata and the property is an own property, fill it in.
        if (!cacheable_metadata || !descriptor->property_offset.has_value() || !shape().is_cacheable())
            return;
        if (phase == PropertyLookupPhase::OwnProperty) {
            *cacheable_metadata = CacheablePropertyMetadata {
                .type = CacheablePropertyMetadata::Type::OwnProperty,
                .property_offset = descriptor->property_offset.value(),
                .prototype = nullptr,
            };
        } else if (phase == PropertyLookupPhase::PrototypeChain) {
            VERIFY(shape().is_prototype_shape());
            VERIFY(shape().prototype_chain_validity()->is_valid());
            *cacheable_metadata = CacheablePropertyMetadata {
                .type = CacheablePropertyMetadata::Type::InPrototypeChain,
                .property_offset = descriptor->property_offset.value(),
                .prototype = this,
            };
        }
    };

    // 3. If IsDataDescriptor(desc) is true, return desc.[[Value]].
    if (descriptor->is_data_descriptor()) {
        update_inline_cache();
        return *descriptor->value;
    }

    // 4. Assert: IsAccessorDescriptor(desc) is true.
    VERIFY(descriptor->is_accessor_descriptor());

    // 5. Let getter be desc.[[Get]].
    auto getter = *descriptor->get;

    // 6. If getter is undefined, return undefined.
    if (!getter)
        return js_undefined();

    update_inline_cache();

    // 7. Return ? Call(getter, Receiver).
    return TRY(call(vm, *getter, receiver));
}

// 10.1.9 [[Set]] ( P, V, Receiver ), https://tc39.es/ecma262/#sec-ordinary-object-internal-methods-and-internal-slots-set-p-v-receiver
// 10.1.9.1 OrdinarySet ( O, P, V, Receiver ), https://tc39.es/ecma262/#sec-ordinaryset
ThrowCompletionOr<bool> Object::internal_set(PropertyKey const& property_key, Value value, Value receiver, CacheablePropertyMetadata* cacheable_metadata)
{
    VERIFY(!value.is_empty());
    VERIFY(!receiver.is_empty());

    // 2. Let ownDesc be ? O.[[GetOwnProperty]](P).
    auto own_descriptor = TRY(internal_get_own_property(property_key));

    // 3. Return ? OrdinarySetWithOwnDescriptor(O, P, V, Receiver, ownDesc).
    return ordinary_set_with_own_descriptor(property_key, value, receiver, own_descriptor, cacheable_metadata);
}

// 10.1.9.2 OrdinarySetWithOwnDescriptor ( O, P, V, Receiver, ownDesc ), https://tc39.es/ecma262/#sec-ordinarysetwithowndescriptor
ThrowCompletionOr<bool> Object::ordinary_set_with_own_descriptor(PropertyKey const& property_key, Value value, Value receiver, Optional<PropertyDescriptor> own_descriptor, CacheablePropertyMetadata* cacheable_metadata)
{
    VERIFY(!value.is_empty());
    VERIFY(!receiver.is_empty());

    auto& vm = this->vm();

    // 1. If ownDesc is undefined, then
    if (!own_descriptor.has_value()) {
        // a. Let parent be ? O.[[GetPrototypeOf]]().
        auto* parent = TRY(internal_get_prototype_of());

        // b. If parent is not null, then
        if (parent) {
            // i. Return ? parent.[[Set]](P, V, Receiver).
            return TRY(parent->internal_set(property_key, value, receiver));
        }
        // c. Else,
        else {
            // i. Set ownDesc to the PropertyDescriptor { [[Value]]: undefined, [[Writable]]: true, [[Enumerable]]: true, [[Configurable]]: true }.
            own_descriptor = PropertyDescriptor {
                .value = js_undefined(),
                .writable = true,
                .enumerable = true,
                .configurable = true,
            };
        }
    }

    // 2. If IsDataDescriptor(ownDesc) is true, then
    if (own_descriptor->is_data_descriptor()) {
        // a. If ownDesc.[[Writable]] is false, return false.
        if (!*own_descriptor->writable)
            return false;

        // b. If Receiver is not an Object, return false.
        if (!receiver.is_object())
            return false;

        auto& receiver_object = receiver.as_object();

        // c. Let existingDescriptor be ? Receiver.[[GetOwnProperty]](P).
        auto existing_descriptor = TRY(receiver_object.internal_get_own_property(property_key));

        // d. If existingDescriptor is not undefined, then
        if (existing_descriptor.has_value()) {
            // i. If IsAccessorDescriptor(existingDescriptor) is true, return false.
            if (existing_descriptor->is_accessor_descriptor())
                return false;

            // ii. If existingDescriptor.[[Writable]] is false, return false.
            if (!*existing_descriptor->writable)
                return false;

            // iii. Let valueDesc be the PropertyDescriptor { [[Value]]: V }.
            auto value_descriptor = PropertyDescriptor { .value = value };

            if (cacheable_metadata && own_descriptor.has_value() && own_descriptor->property_offset.has_value() && shape().is_cacheable()) {
                *cacheable_metadata = CacheablePropertyMetadata {
                    .type = CacheablePropertyMetadata::Type::OwnProperty,
                    .property_offset = own_descriptor->property_offset.value(),
                    .prototype = nullptr,
                };
            }

            // iv. Return ? Receiver.[[DefineOwnProperty]](P, valueDesc).
            return TRY(receiver_object.internal_define_own_property(property_key, value_descriptor, &existing_descriptor));
        }
        // e. Else,
        else {
            // i. Assert: Receiver does not currently have a property P.
            VERIFY(!receiver_object.storage_has(property_key));

            // ii. Return ? CreateDataProperty(Receiver, P, V).
            return TRY(receiver_object.create_data_property(property_key, value));
        }
    }

    // 3. Assert: IsAccessorDescriptor(ownDesc) is true.
    VERIFY(own_descriptor->is_accessor_descriptor());

    // 4. Let setter be ownDesc.[[Set]].
    auto setter = *own_descriptor->set;

    // 5. If setter is undefined, return false.
    if (!setter)
        return false;

    // 6. Perform ? Call(setter, Receiver, « V »).
    (void)TRY(call(vm, *setter, receiver, value));

    // 7. Return true.
    return true;
}

// 10.1.10 [[Delete]] ( P ), https://tc39.es/ecma262/#sec-ordinary-object-internal-methods-and-internal-slots-delete-p
// 10.1.10.1 OrdinaryDelete ( O, P ), https://tc39.es/ecma262/#sec-ordinarydelete
ThrowCompletionOr<bool> Object::internal_delete(PropertyKey const& property_key)
{
    // 1. Let desc be ? O.[[GetOwnProperty]](P).
    auto descriptor = TRY(internal_get_own_property(property_key));

    // 2. If desc is undefined, return true.
    if (!descriptor.has_value())
        return true;

    // 3. If desc.[[Configurable]] is true, then
    if (*descriptor->configurable) {
        // a. Remove the own property with name P from O.
        storage_delete(property_key);

        // b. Return true.
        return true;
    }

    // 4. Return false.
    return false;
}

// 10.1.11 [[OwnPropertyKeys]] ( ), https://tc39.es/ecma262/#sec-ordinary-object-internal-methods-and-internal-slots-ownpropertykeys
ThrowCompletionOr<GC::RootVector<Value>> Object::internal_own_property_keys() const
{
    auto& vm = this->vm();

    // 1. Let keys be a new empty List.
    GC::RootVector<Value> keys { heap() };

    // 2. For each own property key P of O such that P is an array index, in ascending numeric index order, do
    for (auto& entry : m_indexed_properties) {
        // a. Add P as the last element of keys.
        keys.append(PrimitiveString::create(vm, ByteString::number(entry.index())));
    }

    // 3. For each own property key P of O such that Type(P) is String and P is not an array index, in ascending chronological order of property creation, do
    for (auto& it : shape().property_table()) {
        if (it.key.is_string()) {
            // a. Add P as the last element of keys.
            keys.append(it.key.to_value(vm));
        }
    }

    // 4. For each own property key P of O such that Type(P) is Symbol, in ascending chronological order of property creation, do
    for (auto& it : shape().property_table()) {
        if (it.key.is_symbol()) {
            // a. Add P as the last element of keys.
            keys.append(it.key.to_value(vm));
        }
    }

    // 5. Return keys.
    return { move(keys) };
}

// 10.4.7.2 SetImmutablePrototype ( O, V ), https://tc39.es/ecma262/#sec-set-immutable-prototype
ThrowCompletionOr<bool> Object::set_immutable_prototype(Object* prototype)
{
    // 1. Let current be ? O.[[GetPrototypeOf]]().
    auto* current = TRY(internal_get_prototype_of());

    // 2. If SameValue(V, current) is true, return true.
    if (prototype == current)
        return true;

    // 3. Return false.
    return false;
}

static Optional<Object::IntrinsicAccessor> find_intrinsic_accessor(Object const* object, PropertyKey const& property_key)
{
    if (!property_key.is_string())
        return {};

    auto intrinsics = s_intrinsics.find(object);
    if (intrinsics == s_intrinsics.end())
        return {};

    auto accessor_iterator = intrinsics->value.find(property_key.as_string());
    if (accessor_iterator == intrinsics->value.end())
        return {};

    auto accessor = accessor_iterator->value;
    intrinsics->value.remove(accessor_iterator);
    return accessor;
}

Optional<ValueAndAttributes> Object::storage_get(PropertyKey const& property_key) const
{
    Value value;
    PropertyAttributes attributes;
    Optional<u32> property_offset;

    if (property_key.is_number()) {
        auto value_and_attributes = m_indexed_properties.get(property_key.as_number());
        if (!value_and_attributes.has_value())
            return {};
        value = value_and_attributes->value;
        attributes = value_and_attributes->attributes;
    } else {
        auto metadata = shape().lookup(property_key.to_string_or_symbol());
        if (!metadata.has_value())
            return {};

        if (m_has_intrinsic_accessors) {
            if (auto accessor = find_intrinsic_accessor(this, property_key); accessor.has_value())
                const_cast<Object&>(*this).m_storage[metadata->offset] = (*accessor)(shape().realm());
        }

        value = m_storage[metadata->offset];
        attributes = metadata->attributes;
        property_offset = metadata->offset;
    }

    return ValueAndAttributes { .value = value, .attributes = attributes, .property_offset = property_offset };
}

bool Object::storage_has(PropertyKey const& property_key) const
{
    if (property_key.is_number())
        return m_indexed_properties.has_index(property_key.as_number());
    return shape().lookup(property_key.to_string_or_symbol()).has_value();
}

void Object::storage_set(PropertyKey const& property_key, ValueAndAttributes const& value_and_attributes)
{
    auto [value, attributes, _] = value_and_attributes;

    if (property_key.is_number()) {
        auto index = property_key.as_number();
        m_indexed_properties.put(index, value, attributes);
        return;
    }

    if (m_has_intrinsic_accessors && property_key.is_string()) {
        if (auto intrinsics = s_intrinsics.find(this); intrinsics != s_intrinsics.end())
            intrinsics->value.remove(property_key.as_string());
    }

    auto property_key_string_or_symbol = property_key.to_string_or_symbol();
    auto metadata = shape().lookup(property_key_string_or_symbol);

    if (!metadata.has_value()) {
        static constexpr size_t max_transitions_before_converting_to_dictionary = 64;
        if (!m_shape->is_dictionary() && m_shape->property_count() >= max_transitions_before_converting_to_dictionary)
            set_shape(m_shape->create_cacheable_dictionary_transition());

        if (m_shape->is_dictionary())
            m_shape->add_property_without_transition(property_key_string_or_symbol, attributes);
        else
            set_shape(*m_shape->create_put_transition(property_key_string_or_symbol, attributes));
        m_storage.append(value);
        return;
    }

    if (attributes != metadata->attributes) {
        if (m_shape->is_dictionary())
            m_shape->set_property_attributes_without_transition(property_key_string_or_symbol, attributes);
        else
            set_shape(*m_shape->create_configure_transition(property_key_string_or_symbol, attributes));
    }

    m_storage[metadata->offset] = value;
}

void Object::storage_delete(PropertyKey const& property_key)
{
    VERIFY(storage_has(property_key));

    if (property_key.is_number())
        return m_indexed_properties.remove(property_key.as_number());

    if (m_has_intrinsic_accessors && property_key.is_string()) {
        if (auto intrinsics = s_intrinsics.find(this); intrinsics != s_intrinsics.end())
            intrinsics->value.remove(property_key.as_string());
    }

    auto metadata = shape().lookup(property_key.to_string_or_symbol());
    VERIFY(metadata.has_value());

    if (m_shape->is_cacheable_dictionary()) {
        m_shape = m_shape->create_uncacheable_dictionary_transition();
    }
    if (m_shape->is_uncacheable_dictionary()) {
        m_shape->remove_property_without_transition(property_key.to_string_or_symbol(), metadata->offset);
        m_storage.remove(metadata->offset);
        return;
    }
    m_shape = m_shape->create_delete_transition(property_key.to_string_or_symbol());
    m_storage.remove(metadata->offset);
}

void Object::set_prototype(Object* new_prototype)
{
    if (prototype() == new_prototype)
        return;
    m_shape = shape().create_prototype_transition(new_prototype);
}

void Object::define_native_accessor(Realm& realm, PropertyKey const& property_key, Function<ThrowCompletionOr<Value>(VM&)> getter, Function<ThrowCompletionOr<Value>(VM&)> setter, PropertyAttributes attribute)
{
    FunctionObject* getter_function = nullptr;
    if (getter)
        getter_function = NativeFunction::create(realm, move(getter), 0, property_key, &realm, {}, "get"sv);
    FunctionObject* setter_function = nullptr;
    if (setter)
        setter_function = NativeFunction::create(realm, move(setter), 1, property_key, &realm, {}, "set"sv);
    return define_direct_accessor(property_key, getter_function, setter_function, attribute);
}

void Object::define_direct_accessor(PropertyKey const& property_key, FunctionObject* getter, FunctionObject* setter, PropertyAttributes attributes)
{
    auto existing_property = storage_get(property_key).value_or({}).value;
    auto* accessor = existing_property.is_accessor() ? &existing_property.as_accessor() : nullptr;
    if (!accessor) {
        accessor = Accessor::create(vm(), getter, setter);
        define_direct_property(property_key, accessor, attributes);
    } else {
        if (getter)
            accessor->set_getter(getter);
        if (setter)
            accessor->set_setter(setter);
    }
}

void Object::define_intrinsic_accessor(PropertyKey const& property_key, PropertyAttributes attributes, IntrinsicAccessor accessor)
{
    VERIFY(property_key.is_string());

    storage_set(property_key, { {}, attributes });

    m_has_intrinsic_accessors = true;
    auto& intrinsics = s_intrinsics.ensure(this);
    intrinsics.set(property_key.as_string(), move(accessor));
}

// Simple side-effect free property lookup, following the prototype chain. Non-standard.
Value Object::get_without_side_effects(PropertyKey const& property_key) const
{
    auto* object = this;
    while (object) {
        auto value_and_attributes = object->storage_get(property_key);
        if (value_and_attributes.has_value())
            return value_and_attributes->value;
        object = object->prototype();
    }
    return {};
}

void Object::define_native_function(Realm& realm, PropertyKey const& property_key, Function<ThrowCompletionOr<Value>(VM&)> native_function, i32 length, PropertyAttributes attribute, Optional<Bytecode::Builtin> builtin)
{
    auto function = NativeFunction::create(realm, move(native_function), length, property_key, &realm);
    define_direct_property(property_key, function, attribute);
    if (builtin.has_value())
        realm.define_builtin(builtin.value(), function);
}

// 20.1.2.3.1 ObjectDefineProperties ( O, Properties ), https://tc39.es/ecma262/#sec-objectdefineproperties
ThrowCompletionOr<Object*> Object::define_properties(Value properties)
{
    auto& vm = this->vm();

    // 1. Let props be ? ToObject(Properties).
    auto props = TRY(properties.to_object(vm));

    // 2. Let keys be ? props.[[OwnPropertyKeys]]().
    auto keys = TRY(props->internal_own_property_keys());

    struct NameAndDescriptor {
        PropertyKey name;
        PropertyDescriptor descriptor;
    };

    // 3. Let descriptors be a new empty List.
    Vector<NameAndDescriptor> descriptors;

    // 4. For each element nextKey of keys, do
    for (auto& next_key : keys) {
        auto property_key = MUST(PropertyKey::from_value(vm, next_key));

        // a. Let propDesc be ? props.[[GetOwnProperty]](nextKey).
        auto property_descriptor = TRY(props->internal_get_own_property(property_key));

        // b. If propDesc is not undefined and propDesc.[[Enumerable]] is true, then
        if (property_descriptor.has_value() && *property_descriptor->enumerable) {
            // i. Let descObj be ? Get(props, nextKey).
            auto descriptor_object = TRY(props->get(property_key));

            // ii. Let desc be ? ToPropertyDescriptor(descObj).
            auto descriptor = TRY(to_property_descriptor(vm, descriptor_object));

            // iii. Append the pair (a two element List) consisting of nextKey and desc to the end of descriptors.
            descriptors.append({ property_key, descriptor });
        }
    }

    // 5. For each element pair of descriptors, do
    for (auto& [name, descriptor] : descriptors) {
        // a. Let P be the first element of pair.
        // b. Let desc be the second element of pair.

        // c. Perform ? DefinePropertyOrThrow(O, P, desc).
        TRY(define_property_or_throw(name, descriptor));
    }

    // 6. Return O.
    return this;
}

// 14.7.5.9 EnumerateObjectProperties ( O ), https://tc39.es/ecma262/#sec-enumerate-object-properties
Optional<Completion> Object::enumerate_object_properties(Function<Optional<Completion>(Value)> callback) const
{
    // 1. Return an Iterator object (27.1.1.2) whose next method iterates over all the String-valued keys of enumerable properties of O. The iterator object is never directly accessible to ECMAScript code. The mechanics and order of enumerating the properties is not specified but must conform to the rules specified below.
    //    * Returned property keys do not include keys that are Symbols.
    //    * Properties of the target object may be deleted during enumeration.
    //    * A property that is deleted before it is processed is ignored.
    //    * If new properties are added to the target object during enumeration, the newly added properties are not guaranteed to be processed in the active enumeration.
    //    * A property name will be returned at most once in any enumeration.
    //    * Enumerating the properties of the target object includes enumerating properties of its prototype, and the prototype of the prototype, and so on, recursively.
    //    * A property of a prototype is not processed if it has the same name as a property that has already been processed.

    HashTable<FlyString> visited;

    auto const* target = this;
    while (target) {
        auto own_keys = TRY(target->internal_own_property_keys());
        for (auto& key : own_keys) {
            if (!key.is_string())
                continue;
            FlyString property_key = key.as_string().utf8_string();
            if (visited.contains(property_key))
                continue;
            auto descriptor = TRY(target->internal_get_own_property(property_key));
            if (!descriptor.has_value())
                continue;
            visited.set(property_key);
            if (!*descriptor->enumerable)
                continue;
            if (auto completion = callback(key); completion.has_value())
                return completion.release_value();
        }

        target = TRY(target->internal_get_prototype_of());
    };

    return {};
}

void Object::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_shape);
    visitor.visit(m_storage);

    m_indexed_properties.for_each_value([&visitor](auto& value) {
        visitor.visit(value);
    });

    if (m_private_elements) {
        for (auto& private_element : *m_private_elements)
            visitor.visit(private_element.value);
    }
}

// 7.1.1.1 OrdinaryToPrimitive ( O, hint ), https://tc39.es/ecma262/#sec-ordinarytoprimitive
ThrowCompletionOr<Value> Object::ordinary_to_primitive(Value::PreferredType preferred_type) const
{
    VERIFY(preferred_type == Value::PreferredType::String || preferred_type == Value::PreferredType::Number);

    auto& vm = this->vm();

    AK::Array<PropertyKey, 2> method_names = (preferred_type == Value::PreferredType::String)
        // 1. If hint is string, then
        // a. Let methodNames be « "toString", "valueOf" ».
        ? AK::Array { vm.names.toString, vm.names.valueOf }
        // 2. Else,
        // a. Let methodNames be « "valueOf", "toString" ».
        : AK::Array { vm.names.valueOf, vm.names.toString };

    // 3. For each element name of methodNames, do
    for (auto& method_name : method_names) {
        // a. Let method be ? Get(O, name).
        auto method = TRY(get(method_name));

        // b. If IsCallable(method) is true, then
        if (method.is_function()) {
            // i. Let result be ? Call(method, O).
            auto result = TRY(call(vm, method.as_function(), const_cast<Object*>(this)));

            // ii. If Type(result) is not Object, return result.
            if (!result.is_object())
                return result;
        }
    }

    // 4. Throw a TypeError exception.
    return vm.throw_completion<TypeError>(ErrorType::Convert, "object", preferred_type == Value::PreferredType::String ? "string" : "number");
}

void Object::convert_to_prototype_if_needed()
{
    if (shape().is_prototype_shape())
        return;
    set_shape(shape().clone_for_prototype());
}

}
