/*
 * Copyright (c) 2020-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2020-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/QuickSort.h>
#include <AK/TypeCasts.h>
#include <AK/kmalloc.h>
#include <LibJS/Bytecode/PropertyAccess.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Accessor.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/ArrayIteratorPrototype.h>
#include <LibJS/Runtime/ClassFieldDefinition.h>
#include <LibJS/Runtime/ECMAScriptFunctionObject.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/MapIteratorPrototype.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/NativeJavaScriptBackedFunction.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/PropertyDescriptor.h>
#include <LibJS/Runtime/ProxyObject.h>
#include <LibJS/Runtime/SetIteratorPrototype.h>
#include <LibJS/Runtime/Shape.h>
#include <LibJS/Runtime/StringIteratorPrototype.h>
#include <LibJS/Runtime/Value.h>

namespace JS {

GC_DEFINE_ALLOCATOR(Object);

static HashMap<GC::Ptr<Object const>, HashMap<Utf16FlyString, Object::IntrinsicAccessor>> s_intrinsics;

// Heap-allocated named property storage layout:
//   [u32 capacity] [u32 padding] [Value 0] [Value 1] ...
//   m_named_properties points to Value 0.
// For small property counts (<=INLINE_NAMED_PROPERTY_CAPACITY), storage is inline in the Object.
static constexpr u32 HEAP_STORAGE_HEADER_SIZE = sizeof(Value);

static Value* allocate_heap_named_storage(u32 capacity)
{
    VERIFY(capacity > Object::INLINE_NAMED_PROPERTY_CAPACITY);
    auto allocation_size = HEAP_STORAGE_HEADER_SIZE + capacity * sizeof(Value);
    auto* raw = static_cast<u8*>(kmalloc(allocation_size));
    VERIFY(raw);
    *reinterpret_cast<u32*>(raw) = capacity;
    return reinterpret_cast<Value*>(raw + HEAP_STORAGE_HEADER_SIZE);
}

static void free_heap_named_storage(Value* storage)
{
    auto* raw = reinterpret_cast<u8*>(storage) - HEAP_STORAGE_HEADER_SIZE;
    kfree(raw);
}

static u32 heap_named_storage_capacity(Value* storage)
{
    return *reinterpret_cast<u32*>(reinterpret_cast<u8*>(storage) - HEAP_STORAGE_HEADER_SIZE);
}

void Object::ensure_named_storage_capacity(u32 needed)
{
    bool is_inline = named_storage_is_inline();
    u32 old_capacity = is_inline ? INLINE_NAMED_PROPERTY_CAPACITY : heap_named_storage_capacity(m_named_properties);
    if (needed <= old_capacity)
        return;
    u32 new_capacity = max(needed, old_capacity * 2);
    if (is_inline) {
        auto* new_storage = allocate_heap_named_storage(new_capacity);
        memcpy(new_storage, m_inline_named_storage, INLINE_NAMED_PROPERTY_CAPACITY * sizeof(Value));
        for (u32 i = INLINE_NAMED_PROPERTY_CAPACITY; i < new_capacity; ++i)
            new_storage[i] = Value();
        m_named_properties = new_storage;
    } else {
        auto* raw = static_cast<u8*>(krealloc(
            reinterpret_cast<u8*>(m_named_properties) - HEAP_STORAGE_HEADER_SIZE,
            HEAP_STORAGE_HEADER_SIZE + new_capacity * sizeof(Value)));
        VERIFY(raw);
        *reinterpret_cast<u32*>(raw) = new_capacity;
        m_named_properties = reinterpret_cast<Value*>(raw + HEAP_STORAGE_HEADER_SIZE);
        for (u32 i = old_capacity; i < new_capacity; ++i)
            m_named_properties[i] = Value();
    }
}

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
{
    if (may_interfere_with_indexed_property_access == MayInterfereWithIndexedPropertyAccess::Yes)
        set_may_interfere_with_indexed_property_access();
    // This is the global object
    m_shape = heap().allocate<Shape>(realm);
}

Object::Object(ConstructWithoutPrototypeTag, Realm& realm, MayInterfereWithIndexedPropertyAccess may_interfere_with_indexed_property_access)
{
    if (may_interfere_with_indexed_property_access == MayInterfereWithIndexedPropertyAccess::Yes)
        set_may_interfere_with_indexed_property_access();
    m_shape = heap().allocate<Shape>(realm);
}

Object::Object(Realm& realm, Object* prototype, MayInterfereWithIndexedPropertyAccess may_interfere_with_indexed_property_access)
{
    if (may_interfere_with_indexed_property_access == MayInterfereWithIndexedPropertyAccess::Yes)
        set_may_interfere_with_indexed_property_access();
    m_shape = realm.intrinsics().empty_object_shape();
    VERIFY(m_shape);
    if (prototype != nullptr)
        set_prototype(prototype);
}

Object::Object(ConstructWithPrototypeTag, Object& prototype, MayInterfereWithIndexedPropertyAccess may_interfere_with_indexed_property_access)
{
    if (may_interfere_with_indexed_property_access == MayInterfereWithIndexedPropertyAccess::Yes)
        set_may_interfere_with_indexed_property_access();
    m_shape = prototype.shape().realm().intrinsics().empty_object_shape();
    VERIFY(m_shape);
    set_prototype(&prototype);
}

Object::Object(Shape& shape, MayInterfereWithIndexedPropertyAccess may_interfere_with_indexed_property_access)
    : m_shape(&shape)
{
    if (may_interfere_with_indexed_property_access == MayInterfereWithIndexedPropertyAccess::Yes)
        set_may_interfere_with_indexed_property_access();
    if (shape.property_count() > 0)
        ensure_named_storage_capacity(shape.property_count());
}

Object::~Object()
{
    free_indexed_elements();
    if (has_intrinsic_accessors())
        s_intrinsics.remove(this);
    if (!named_storage_is_inline())
        free_heap_named_storage(m_named_properties);
}

void Object::initialize(Realm& realm)
{
    Base::initialize(realm);
}

void Object::unsafe_set_shape(Shape& shape)
{
    m_shape = shape;
    ensure_named_storage_capacity(shape.property_count());
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

// 7.3.2 Get ( O, P ), https://tc39.es/ecma262/#sec-get-o-p
ThrowCompletionOr<Value> Object::get(PropertyKey const& property_key, Bytecode::PropertyLookupCache& cache) const
{
    // 1. Return ? O.[[Get]](P, O).
    return TRY(Value(this).get(vm(), property_key, cache));
}

// NOTE: 7.3.3 GetV ( V, P ) is implemented as Value::get().

// 7.3.4 Set ( O, P, V, Throw ), https://tc39.es/ecma262/#sec-set-o-p-v-throw
ThrowCompletionOr<void> Object::set(PropertyKey const& property_key, Value value, ShouldThrowExceptions throw_exceptions)
{
    auto& vm = this->vm();

    VERIFY(!value.is_special_empty_value());

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

ThrowCompletionOr<void> Object::set(PropertyKey const& property_key, Value value, Bytecode::PropertyLookupCache& cache)
{
    Strict strict = Strict::No;
    if (auto function = vm().running_execution_context().function; function && function->is_strict_mode())
        strict = Strict::Yes;
    return Bytecode::put_by_property_key(vm(), this, this, value, {}, property_key, Bytecode::PutKind::Normal, strict, &cache);
}

// 7.3.5 CreateDataProperty ( O, P, V ), https://tc39.es/ecma262/#sec-createdataproperty
ThrowCompletionOr<bool> Object::create_data_property(PropertyKey const& property_key, Value value, Optional<u32>* new_property_offset)
{
    // 1. Let newDesc be the PropertyDescriptor { [[Value]]: V, [[Writable]]: true, [[Enumerable]]: true, [[Configurable]]: true }.
    auto new_descriptor = PropertyDescriptor {
        .value = value,
        .writable = true,
        .enumerable = true,
        .configurable = true,
    };

    // 2. Return ? O.[[DefineOwnProperty]](P, newDesc).
    auto result = internal_define_own_property(property_key, new_descriptor);
    if (new_property_offset && new_descriptor.property_offset.has_value())
        *new_property_offset = new_descriptor.property_offset.value();
    return result;
}

// 7.3.6 CreateMethodProperty ( O, P, V ), https://tc39.es/ecma262/#sec-createmethodproperty
void Object::create_method_property(PropertyKey const& property_key, Value value)
{
    VERIFY(!value.is_special_empty_value());

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

    VERIFY(!value.is_special_empty_value());

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
    VERIFY(!value.is_special_empty_value());

    // 1. Assert: O is an ordinary, extensible object with no non-configurable properties.

    // 2. Let newDesc be the PropertyDescriptor { [[Value]]: V, [[Writable]]: true, [[Enumerable]]: false, [[Configurable]]: true }.
    auto new_description = PropertyDescriptor { .value = value, .writable = true, .enumerable = false, .configurable = true };

    // 3. Perform ! DefinePropertyOrThrow(O, P, newDesc).
    MUST(define_property_or_throw(property_key, new_description));

    // 4. Return unused.
}

// 7.3.9 DefinePropertyOrThrow ( O, P, desc ), https://tc39.es/ecma262/#sec-definepropertyorthrow
ThrowCompletionOr<void> Object::define_property_or_throw(PropertyKey const& property_key, PropertyDescriptor& property_descriptor)
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
            PropertyDescriptor descriptor { .configurable = false };
            TRY(define_property_or_throw(property_key, descriptor));
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

    // 2. Let properties be a new empty List.
    auto properties = GC::RootVector<Value> { heap() };
    properties.ensure_capacity(own_properties_count());

    auto& pre_iteration_shape = shape();
    TRY(for_each_own_property_with_enumerability([&](PropertyKey const& property_key, bool enumerable) -> ThrowCompletionOr<void> {
        // a. If Type(key) is String, then
        // i. Let desc be ? O.[[GetOwnProperty]](key).
        // ii. If desc is not undefined and desc.[[Enumerable]] is true, then
        // NOTE: If the object's shape has been mutated during iteration through own properties
        //       by executing a getter, we can no longer assume that subsequent properties
        //       are still present and enumerable.
        if (&shape() == &pre_iteration_shape) {
            if (!enumerable)
                return {};
        } else {
            auto descriptor = TRY(internal_get_own_property(property_key));
            if (!descriptor.has_value() || !*descriptor->enumerable)
                return {};
        }

        // 1. If kind is key, append key to properties.
        if (kind == PropertyKind::Key) {
            // 1. If kind is key, append key to properties.
            properties.append(property_key.to_value(vm));
            return {};
        }

        // 2. Else,
        // a. Let value be ? Get(O, key).
        auto value = TRY(get(property_key));

        // b. If kind is value, append value to properties.
        if (kind == PropertyKind::Value) {
            properties.append(value);
            return {};
        }

        // c. Else,
        // i. Assert: kind is key+value.
        VERIFY(kind == PropertyKind::KeyAndValue);

        // ii. Let entry be CreateArrayFromList(« key, value »).
        auto entry = Array::create_from(realm, { property_key.to_value(vm), value });

        // iii. Append entry to properties.
        properties.append(entry);

        return {};
    }));

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
    if (!initializer.has<Empty>()) {
        // OPTIMIZATION: If the initializer is a value (from a literal), we can skip the call.
        if (auto const* initializer_value = initializer.get_pointer<Value>()) {
            init_value = *initializer_value;
        } else {
            // a. Let initValue be ? Call(initializer, receiver).
            init_value = TRY(call(vm, *initializer.get<GC::Ref<ECMAScriptFunctionObject>>(), this));
        }
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
    // AD-HOC: Avoid lazy instantiation of ECMAScriptFunctionObject::ClassData.
    if (!constructor.has_class_data())
        return {};

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
    if (!extensible())
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
    return extensible();
}

// 10.1.4 [[PreventExtensions]] ( ), https://tc39.es/ecma262/#sec-ordinary-object-internal-methods-and-internal-slots-preventextensions
ThrowCompletionOr<bool> Object::internal_prevent_extensions()
{
    // 1. Set O.[[Extensible]] to false.
    set_extensible(false);

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
        descriptor.value = value;

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
ThrowCompletionOr<bool> Object::internal_define_own_property(PropertyKey const& property_key, PropertyDescriptor& property_descriptor, Optional<PropertyDescriptor>* precomputed_get_own_property)
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
ThrowCompletionOr<Value> Object::internal_get(PropertyKey const& property_key, Value receiver, CacheableGetPropertyMetadata* cacheable_metadata, PropertyLookupPhase phase) const
{
    VERIFY(!receiver.is_special_empty_value());

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
        if (!cacheable_metadata || !descriptor->property_offset.has_value())
            return;
        if (phase == PropertyLookupPhase::OwnProperty) {
            *cacheable_metadata = CacheableGetPropertyMetadata {
                .type = CacheableGetPropertyMetadata::Type::GetOwnProperty,
                .property_offset = descriptor->property_offset.value(),
                .prototype = nullptr,
            };
        } else if (phase == PropertyLookupPhase::PrototypeChain) {
            VERIFY(shape().is_prototype_shape());
            VERIFY(shape().prototype_chain_validity()->is_valid());
            *cacheable_metadata = CacheableGetPropertyMetadata {
                .type = CacheableGetPropertyMetadata::Type::GetPropertyInPrototypeChain,
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
ThrowCompletionOr<bool> Object::internal_set(PropertyKey const& property_key, Value value, Value receiver, CacheableSetPropertyMetadata* cacheable_metadata, PropertyLookupPhase phase)
{
    VERIFY(!value.is_special_empty_value());
    VERIFY(!receiver.is_special_empty_value());

    // 2. Let ownDesc be ? O.[[GetOwnProperty]](P).
    auto own_descriptor = TRY(internal_get_own_property(property_key));

    // 3. Return ? OrdinarySetWithOwnDescriptor(O, P, V, Receiver, ownDesc).
    return ordinary_set_with_own_descriptor(property_key, value, receiver, own_descriptor, cacheable_metadata, phase);
}

// 10.1.9.2 OrdinarySetWithOwnDescriptor ( O, P, V, Receiver, ownDesc ), https://tc39.es/ecma262/#sec-ordinarysetwithowndescriptor
ThrowCompletionOr<bool> Object::ordinary_set_with_own_descriptor(PropertyKey const& property_key, Value value, Value receiver, Optional<PropertyDescriptor> own_descriptor, CacheableSetPropertyMetadata* cacheable_metadata, PropertyLookupPhase phase)
{
    VERIFY(!value.is_special_empty_value());
    VERIFY(!receiver.is_special_empty_value());

    auto& vm = this->vm();
    bool own_descriptor_was_undefined = !own_descriptor.has_value();

    // 1. If ownDesc is undefined, then
    if (!own_descriptor.has_value()) {
        // a. Let parent be ? O.[[GetPrototypeOf]]().
        auto* parent = TRY(internal_get_prototype_of());

        // b. If parent is not null, then
        if (parent) {
            // i. Return ? parent.[[Set]](P, V, Receiver).
            return TRY(parent->internal_set(property_key, value, receiver, cacheable_metadata, PropertyLookupPhase::PrototypeChain));
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

    auto update_inline_cache_for_property_change = [&] {
        // Non-standard: If the caller has requested cacheable metadata and the property is an own property, fill it in.
        if (!cacheable_metadata || !own_descriptor->property_offset.has_value())
            return;
        if (phase == PropertyLookupPhase::OwnProperty) {
            *cacheable_metadata = CacheableSetPropertyMetadata {
                .type = CacheableSetPropertyMetadata::Type::ChangeOwnProperty,
                .property_offset = own_descriptor->property_offset.value(),
                .prototype = nullptr,
            };
        } else if (phase == PropertyLookupPhase::PrototypeChain) {
            VERIFY(shape().is_prototype_shape());
            VERIFY(shape().prototype_chain_validity()->is_valid());
            *cacheable_metadata = CacheableSetPropertyMetadata {
                .type = CacheableSetPropertyMetadata::Type::ChangePropertyInPrototypeChain,
                .property_offset = own_descriptor->property_offset.value(),
                .prototype = this,
            };
        }
    };

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
        // OPTIMIZATION: If we were called with an ownDescriptor, and receiver == this, don't do [[GetOwnProperty]] again.
        Optional<PropertyDescriptor> existing_descriptor;
        if (!own_descriptor_was_undefined && &receiver_object == this)
            existing_descriptor = own_descriptor;
        else
            existing_descriptor = TRY(receiver_object.internal_get_own_property(property_key));

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

            // NOTE: We don't cache non-setter properties in the prototype chain, as that's a weird
            //       use-case, and doesn't seem like something in need of optimization.
            if (phase == PropertyLookupPhase::OwnProperty)
                update_inline_cache_for_property_change();

            // iv. Return ? Receiver.[[DefineOwnProperty]](P, valueDesc).
            return TRY(receiver_object.internal_define_own_property(property_key, value_descriptor, &existing_descriptor));
        }
        // e. Else,
        else {
            // i. Assert: Receiver does not currently have a property P.
            VERIFY(!receiver_object.storage_has(property_key));

            // ii. Return ? CreateDataProperty(Receiver, P, V).
            Optional<u32> new_property_offset;
            auto result = TRY(receiver_object.create_data_property(property_key, value, &new_property_offset));
            auto& receiver_shape = receiver_object.shape();
            if (cacheable_metadata && new_property_offset.has_value() && !receiver_shape.is_dictionary()) {
                VERIFY(!property_key.is_number());
                *cacheable_metadata = CacheableSetPropertyMetadata {
                    .type = CacheableSetPropertyMetadata::Type::AddOwnProperty,
                    .property_offset = *new_property_offset,
                    .prototype = receiver_object.prototype(),
                };
            }
            return result;
        }
    }

    // 3. Assert: IsAccessorDescriptor(ownDesc) is true.
    VERIFY(own_descriptor->is_accessor_descriptor());

    // 4. Let setter be ownDesc.[[Set]].
    auto setter = *own_descriptor->set;

    // 5. If setter is undefined, return false.
    if (!setter)
        return false;

    update_inline_cache_for_property_change();

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
    {
        auto indices = indexed_indices();
        for (auto index : indices) {
            // a. Add P as the last element of keys.
            keys.append(PrimitiveString::create_from_unsigned_integer(vm, index));
        }
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
        auto value_and_attributes = indexed_get(property_key.as_number());
        if (value_and_attributes.has_value()) {
            value = value_and_attributes->value;
            attributes = value_and_attributes->attributes;
            return ValueAndAttributes { .value = value, .attributes = attributes, .property_offset = {} };
        }
    }
    {
        auto metadata = shape().lookup(property_key);
        if (!metadata.has_value())
            return {};

        if (has_intrinsic_accessors()) {
            if (auto accessor = find_intrinsic_accessor(this, property_key); accessor.has_value())
                const_cast<Object&>(*this).m_named_properties[metadata->offset] = (*accessor)(shape().realm());
        }

        value = m_named_properties[metadata->offset];
        attributes = metadata->attributes;
        property_offset = metadata->offset;
    }

    return ValueAndAttributes { .value = value, .attributes = attributes, .property_offset = property_offset };
}

bool Object::storage_has(PropertyKey const& property_key) const
{
    if (property_key.is_number() && indexed_has(property_key.as_number()))
        return true;
    return shape().lookup(property_key).has_value();
}

Optional<u32> Object::storage_set(PropertyKey const& property_key, ValueAndAttributes const& value_and_attributes)
{
    auto [value, attributes, _] = value_and_attributes;

    if (property_key.is_number()) {
        // If this numeric key is already in indexed storage, or not yet in named storage, use indexed storage.
        if (indexed_has(property_key.as_number()) || !shape().lookup(property_key).has_value()) {
            indexed_put(property_key.as_number(), value, attributes);
            return {};
        }
        // Otherwise, fall through to named property handling below.
    }

    if (has_intrinsic_accessors() && property_key.is_string()) {
        if (auto intrinsics = s_intrinsics.find(this); intrinsics != s_intrinsics.end())
            intrinsics->value.remove(property_key.as_string());
    }

    auto metadata = shape().lookup(property_key);

    if (!metadata.has_value()) {
        static constexpr size_t max_transitions_before_converting_to_dictionary = 64;
        if (!m_shape->is_dictionary() && m_shape->property_count() >= max_transitions_before_converting_to_dictionary)
            set_shape(m_shape->create_dictionary_transition());

        if (m_shape->is_dictionary())
            m_shape->add_property_without_transition(property_key, attributes);
        else
            set_shape(*m_shape->create_put_transition(property_key, attributes));
        u32 new_offset = shape().property_count() - 1;
        ensure_named_storage_capacity(shape().property_count());
        m_named_properties[new_offset] = value;
        return new_offset;
    }

    if (attributes != metadata->attributes) {
        if (m_shape->is_dictionary())
            m_shape->set_property_attributes_without_transition(property_key, attributes);
        else
            set_shape(*m_shape->create_configure_transition(property_key, attributes));
    }

    m_named_properties[metadata->offset] = value;
    return metadata->offset;
}

void Object::storage_delete(PropertyKey const& property_key)
{
    VERIFY(storage_has(property_key));

    if (property_key.is_number() && indexed_has(property_key.as_number()))
        return indexed_delete(property_key.as_number());

    if (has_intrinsic_accessors() && property_key.is_string()) {
        if (auto intrinsics = s_intrinsics.find(this); intrinsics != s_intrinsics.end())
            intrinsics->value.remove(property_key.as_string());
    }

    auto metadata = shape().lookup(property_key);
    VERIFY(metadata.has_value());

    if (m_shape->is_dictionary()) {
        m_shape->remove_property_without_transition(property_key, metadata->offset);
    } else {
        m_shape = m_shape->create_delete_transition(property_key);
    }
    // Shift remaining properties down to fill the gap.
    u32 remaining = shape().property_count() - metadata->offset;
    if (remaining > 0)
        memmove(&m_named_properties[metadata->offset], &m_named_properties[metadata->offset + 1], remaining * sizeof(Value));
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
        getter_function = NativeFunction::create(realm, move(getter), 0, property_key, &realm, "get"sv);
    FunctionObject* setter_function = nullptr;
    if (setter)
        setter_function = NativeFunction::create(realm, move(setter), 1, property_key, &realm, "set"sv);
    define_direct_accessor(property_key, getter_function, setter_function, attribute);
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

    (void)storage_set(property_key, { {}, attributes });

    set_has_intrinsic_accessors();
    auto& intrinsics = s_intrinsics.ensure(this);
    intrinsics.set(property_key.as_string(), move(accessor));
}

ThrowCompletionOr<void> Object::for_each_own_property_with_enumerability(Function<ThrowCompletionOr<void>(PropertyKey const&, bool)>&& callback) const
{
    auto& vm = this->vm();
    if (eligible_for_own_property_enumeration_fast_path()) {
        struct OwnKey {
            PropertyKey property_key;
            bool enumerable;
        };
        GC::ConservativeVector<OwnKey> keys { heap() };
        keys.ensure_capacity(indexed_real_size() + shape().property_count() + (has_magical_length_property() ? 1 : 0));

        {
            auto indices = indexed_indices();
            for (auto index : indices) {
                bool enumerable = true;
                if (m_indexed_storage_kind == IndexedStorageKind::Dictionary) {
                    auto result = indexed_dictionary()->get(index);
                    if (result.has_value())
                        enumerable = result->attributes.is_enumerable();
                }
                keys.unchecked_append({ PropertyKey(index), enumerable });
            }
        }

        if (has_magical_length_property())
            keys.unchecked_append({ PropertyKey(vm.names.length), false });

        for (auto const& [property_key, metadata] : shape().property_table()) {
            if (!property_key.is_string())
                continue;
            keys.unchecked_append({ property_key, metadata.attributes.is_enumerable() });
        }

        for (auto& key : keys)
            TRY(callback(key.property_key, key.enumerable));
    } else {
        auto keys = TRY(internal_own_property_keys());
        for (auto& key : keys) {
            auto property_key = TRY(PropertyKey::from_value(vm, key));
            if (property_key.is_symbol())
                continue;
            auto descriptor = TRY(internal_get_own_property(property_key));
            bool enumerable = false;
            if (descriptor.has_value())
                enumerable = *descriptor->enumerable;
            TRY(callback(property_key, enumerable));
        }
    }
    return {};
}

size_t Object::own_properties_count() const
{
    return indexed_real_size() + shape().property_table().size() + (has_magical_length_property() ? 1 : 0);
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
    auto function = NativeFunction::create(realm, move(native_function), length, property_key, &realm, {}, builtin);
    define_direct_property(property_key, function, attribute);
}

void Object::define_native_javascript_backed_function(PropertyKey const& property_key, GC::Ref<NativeJavaScriptBackedFunction> function, i32, PropertyAttributes attributes)
{
    define_direct_property(property_key, function, attributes);
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

    HashTable<Utf16FlyString> visited;

    auto const* target = this;
    while (target) {
        auto own_keys = TRY(target->internal_own_property_keys());
        for (auto& key : own_keys) {
            if (!key.is_string())
                continue;
            Utf16FlyString property_key = key.as_string().utf16_string();
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
    if (auto count = shape().property_count())
        visitor.visit(Span<Value> { m_named_properties, count });

    switch (m_indexed_storage_kind) {
    case IndexedStorageKind::None:
        break;
    case IndexedStorageKind::Packed:
        for (u32 i = 0; i < m_indexed_array_like_size; ++i)
            visitor.visit(m_indexed_elements[i]);
        break;
    case IndexedStorageKind::Holey:
        for (u32 i = 0, available_elements = min(m_indexed_array_like_size, indexed_elements_capacity()); i < available_elements; ++i) {
            if (!m_indexed_elements[i].is_special_empty_value())
                visitor.visit(m_indexed_elements[i]);
        }
        break;
    case IndexedStorageKind::Dictionary:
        indexed_dictionary()->visit_edges(visitor);
        break;
    }

    if (m_private_elements) {
        for (auto& private_element : *m_private_elements)
            private_element.visit_edges(visitor);
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
        Value method;
        if (method_name == vm.names.toString) {
            static Bytecode::StaticPropertyLookupCache cache;
            method = TRY(get(method_name, cache));
        } else {
            ASSERT(method_name == vm.names.valueOf);
            static Bytecode::StaticPropertyLookupCache cache;
            method = TRY(get(method_name, cache));
        }

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

// Indexed property storage implementation

static constexpr size_t SPARSE_ARRAY_HOLE_THRESHOLD = 200;

GenericIndexedPropertyStorage* Object::indexed_dictionary() const
{
    VERIFY(m_indexed_storage_kind == IndexedStorageKind::Dictionary);
    return reinterpret_cast<GenericIndexedPropertyStorage*>(m_indexed_elements);
}

u32 Object::indexed_elements_capacity() const
{
    if (!m_indexed_elements)
        return 0;
    VERIFY(m_indexed_storage_kind == IndexedStorageKind::Packed || m_indexed_storage_kind == IndexedStorageKind::Holey);
    // Capacity is stored as a u32 at (m_indexed_elements - sizeof(u64))
    return *reinterpret_cast<u32 const*>(reinterpret_cast<u8 const*>(m_indexed_elements) - sizeof(u64));
}

static Value* allocate_indexed_elements(u32 capacity)
{
    // Layout: [u32 capacity] [u32 padding] [Value 0] [Value 1] ...
    auto allocation_size = sizeof(u64) + capacity * sizeof(Value);
    auto* raw = static_cast<u8*>(kmalloc(allocation_size));
    VERIFY(raw);
    *reinterpret_cast<u32*>(raw) = capacity;
    *reinterpret_cast<u32*>(raw + sizeof(u32)) = 0; // padding
    auto* elements = reinterpret_cast<Value*>(raw + sizeof(u64));
    for (u32 i = 0; i < capacity; ++i)
        new (&elements[i]) Value(js_special_empty_value());
    return elements;
}

static void deallocate_indexed_elements(Value* elements)
{
    if (!elements)
        return;
    auto* raw = reinterpret_cast<u8*>(elements) - sizeof(u64);
    kfree(raw);
}

void Object::free_indexed_elements()
{
    if (m_indexed_storage_kind == IndexedStorageKind::Dictionary) {
        delete indexed_dictionary();
    } else {
        deallocate_indexed_elements(m_indexed_elements);
    }
    m_indexed_elements = nullptr;
    m_indexed_storage_kind = IndexedStorageKind::None;
    m_indexed_array_like_size = 0;
}

void Object::ensure_indexed_elements(u32 needed_capacity)
{
    if (m_indexed_elements && indexed_elements_capacity() >= needed_capacity)
        return;
    grow_indexed_elements(needed_capacity);
}

void Object::grow_indexed_elements(u32 needed_capacity)
{
    // Grow by at least 50% to reduce copying during dense fills.
    u32 old_capacity = m_indexed_elements ? indexed_elements_capacity() : 0;
    u32 new_capacity = max(needed_capacity, old_capacity + old_capacity / 2);
    new_capacity = max(new_capacity, static_cast<u32>(8));

    auto* new_elements = allocate_indexed_elements(new_capacity);

    if (m_indexed_elements) {
        u32 copy_count = min(old_capacity, needed_capacity);
        for (u32 i = 0; i < copy_count; ++i)
            new_elements[i] = m_indexed_elements[i];
        deallocate_indexed_elements(m_indexed_elements);
    }

    m_indexed_elements = new_elements;
}

void Object::transition_to_dictionary()
{
    auto* dict = new GenericIndexedPropertyStorage();

    if (m_indexed_storage_kind == IndexedStorageKind::Packed || m_indexed_storage_kind == IndexedStorageKind::Holey) {
        // Transfer existing elements
        u32 count = min(m_indexed_array_like_size, indexed_elements_capacity());
        for (u32 i = 0; i < count; ++i) {
            auto value = m_indexed_elements[i];
            if (!value.is_special_empty_value())
                dict->put(i, value, default_attributes);
        }
        deallocate_indexed_elements(m_indexed_elements);
    }

    // Set the array_like_size on the dictionary
    dict->set_array_like_size(m_indexed_array_like_size);

    m_indexed_elements = reinterpret_cast<Value*>(dict);
    m_indexed_storage_kind = IndexedStorageKind::Dictionary;
}

Optional<ValueAndAttributes> Object::indexed_get(u32 index) const
{
    switch (m_indexed_storage_kind) {
    case IndexedStorageKind::None:
        return {};
    case IndexedStorageKind::Packed:
        if (index >= m_indexed_array_like_size)
            return {};
        return ValueAndAttributes { m_indexed_elements[index], default_attributes };
    case IndexedStorageKind::Holey:
        if (index >= m_indexed_array_like_size)
            return {};
        if (index >= indexed_elements_capacity())
            return {};
        if (m_indexed_elements[index].is_special_empty_value())
            return {};
        return ValueAndAttributes { m_indexed_elements[index], default_attributes };
    case IndexedStorageKind::Dictionary:
        return indexed_dictionary()->get(index);
    }
    VERIFY_NOT_REACHED();
}

void Object::indexed_put(u32 index, Value value, PropertyAttributes attributes)
{
    bool const storing_hole = value.is_special_empty_value();
    u32 materialized_elements = 0;
    if (m_indexed_storage_kind == IndexedStorageKind::Packed || m_indexed_storage_kind == IndexedStorageKind::Holey)
        materialized_elements = min(m_indexed_array_like_size, indexed_elements_capacity());

    if (m_indexed_storage_kind == IndexedStorageKind::Dictionary) {
        indexed_dictionary()->put(index, value, attributes);
        m_indexed_array_like_size = indexed_dictionary()->array_like_size();
        return;
    }

    // Non-default attributes require Dictionary mode
    if (attributes != default_attributes) {
        if (m_indexed_storage_kind != IndexedStorageKind::Dictionary)
            transition_to_dictionary();
        indexed_dictionary()->put(index, value, attributes);
        m_indexed_array_like_size = indexed_dictionary()->array_like_size();
        return;
    }

    // Check for sparse threshold
    if (index > materialized_elements + SPARSE_ARRAY_HOLE_THRESHOLD) {
        if (m_indexed_storage_kind != IndexedStorageKind::Dictionary)
            transition_to_dictionary();
        indexed_dictionary()->put(index, value, attributes);
        m_indexed_array_like_size = indexed_dictionary()->array_like_size();
        return;
    }

    if (m_indexed_storage_kind == IndexedStorageKind::None) {
        m_indexed_storage_kind = storing_hole || index > 0 ? IndexedStorageKind::Holey : IndexedStorageKind::Packed;
        u32 needed = index + 1;
        ensure_indexed_elements(needed);
        m_indexed_elements[index] = value;
        m_indexed_array_like_size = max(m_indexed_array_like_size, index + 1);
        return;
    }

    // Packed or Holey
    if (index >= materialized_elements)
        ensure_indexed_elements(index + 1);

    if (index >= m_indexed_array_like_size) {
        // Growing
        u32 new_size = index + 1;

        if (m_indexed_storage_kind == IndexedStorageKind::Packed
            && (index > m_indexed_array_like_size || storing_hole)) {
            // Gap created
            m_indexed_storage_kind = IndexedStorageKind::Holey;
        }

        m_indexed_array_like_size = new_size;
    }

    if (m_indexed_storage_kind == IndexedStorageKind::Packed && storing_hole)
        m_indexed_storage_kind = IndexedStorageKind::Holey;

    m_indexed_elements[index] = value;

    // Promote Holey -> Packed when filling the last hole.
    // Only check when writing to the last index to avoid O(N^2) scanning.
    if (m_indexed_storage_kind == IndexedStorageKind::Holey && index == m_indexed_array_like_size - 1) {
        bool has_holes = false;
        for (u32 i = 0, available_elements = min(m_indexed_array_like_size, indexed_elements_capacity()); i < available_elements; ++i) {
            if (m_indexed_elements[i].is_special_empty_value()) {
                has_holes = true;
                break;
            }
        }
        if (!has_holes && indexed_elements_capacity() >= m_indexed_array_like_size)
            m_indexed_storage_kind = IndexedStorageKind::Packed;
    }
}

bool Object::indexed_has(u32 index) const
{
    switch (m_indexed_storage_kind) {
    case IndexedStorageKind::None:
        return false;
    case IndexedStorageKind::Packed:
        return index < m_indexed_array_like_size;
    case IndexedStorageKind::Holey:
        return index < m_indexed_array_like_size
            && index < indexed_elements_capacity()
            && !m_indexed_elements[index].is_special_empty_value();
    case IndexedStorageKind::Dictionary:
        return indexed_dictionary()->has_index(index);
    }
    VERIFY_NOT_REACHED();
}

void Object::indexed_delete(u32 index)
{
    switch (m_indexed_storage_kind) {
    case IndexedStorageKind::None:
        return;
    case IndexedStorageKind::Packed:
        VERIFY(index < m_indexed_array_like_size);
        m_indexed_elements[index] = js_special_empty_value();
        m_indexed_storage_kind = IndexedStorageKind::Holey;
        break;
    case IndexedStorageKind::Holey:
        VERIFY(index < m_indexed_array_like_size);
        if (index >= indexed_elements_capacity())
            return;
        m_indexed_elements[index] = js_special_empty_value();
        break;
    case IndexedStorageKind::Dictionary:
        indexed_dictionary()->remove(index);
        break;
    }
}

bool Object::set_indexed_array_like_size(size_t new_size)
{
    if (new_size == m_indexed_array_like_size)
        return true;

    if (m_indexed_storage_kind == IndexedStorageKind::Dictionary) {
        bool result = indexed_dictionary()->set_array_like_size(new_size);
        m_indexed_array_like_size = indexed_dictionary()->array_like_size();
        return result;
    }

    VERIFY(new_size <= NumericLimits<u32>::max());

    u32 old_size = m_indexed_array_like_size;
    auto new_size_u32 = static_cast<u32>(new_size);

    if (m_indexed_storage_kind == IndexedStorageKind::None) {
        if (new_size_u32 == 0)
            return true;
        m_indexed_storage_kind = IndexedStorageKind::Holey;
        m_indexed_array_like_size = new_size_u32;
        return true;
    }

    if (new_size_u32 > old_size) {
        if (m_indexed_storage_kind == IndexedStorageKind::Packed)
            m_indexed_storage_kind = IndexedStorageKind::Holey;
        m_indexed_array_like_size = new_size_u32;
        return true;
    }

    // Shrinking
    if (new_size_u32 < old_size) {
        u32 capacity = indexed_elements_capacity();
        for (u32 i = new_size_u32; i < min(old_size, capacity); ++i)
            m_indexed_elements[i] = js_special_empty_value();
        m_indexed_array_like_size = new_size_u32;
    }

    return true;
}

void Object::indexed_append(Value value, PropertyAttributes attributes)
{
    indexed_put(m_indexed_array_like_size, value, attributes);
}

ValueAndAttributes Object::indexed_take_first()
{
    if (m_indexed_storage_kind == IndexedStorageKind::Dictionary) {
        auto result = indexed_dictionary()->take_first();
        m_indexed_array_like_size = indexed_dictionary()->array_like_size();
        return result;
    }

    VERIFY(m_indexed_array_like_size > 0);
    if (m_indexed_storage_kind == IndexedStorageKind::None) {
        --m_indexed_array_like_size;
        return {};
    }

    auto available_elements = min(m_indexed_array_like_size, indexed_elements_capacity());
    auto first = available_elements > 0 ? m_indexed_elements[0] : js_special_empty_value();

    // Shift all elements left
    for (u32 i = 0; i + 1 < available_elements; ++i)
        m_indexed_elements[i] = m_indexed_elements[i + 1];

    m_indexed_array_like_size--;
    if (available_elements > 0)
        m_indexed_elements[available_elements - 1] = js_special_empty_value();

    return { first, default_attributes };
}

ValueAndAttributes Object::indexed_take_last()
{
    if (m_indexed_storage_kind == IndexedStorageKind::Dictionary) {
        auto result = indexed_dictionary()->take_last();
        m_indexed_array_like_size = indexed_dictionary()->array_like_size();
        return result;
    }

    VERIFY(m_indexed_array_like_size > 0);
    m_indexed_array_like_size--;
    if (m_indexed_storage_kind == IndexedStorageKind::None)
        return {};
    if (m_indexed_array_like_size >= indexed_elements_capacity())
        return {};

    auto last = m_indexed_elements[m_indexed_array_like_size];
    m_indexed_elements[m_indexed_array_like_size] = js_special_empty_value();

    if (last.is_special_empty_value())
        return {};
    return { last, default_attributes };
}

size_t Object::indexed_real_size() const
{
    switch (m_indexed_storage_kind) {
    case IndexedStorageKind::None:
        return 0;
    case IndexedStorageKind::Packed:
        return m_indexed_array_like_size;
    case IndexedStorageKind::Holey: {
        size_t count = 0;
        for (u32 i = 0, available_elements = min(m_indexed_array_like_size, indexed_elements_capacity()); i < available_elements; ++i) {
            if (!m_indexed_elements[i].is_special_empty_value())
                ++count;
        }
        return count;
    }
    case IndexedStorageKind::Dictionary:
        return indexed_dictionary()->size();
    }
    VERIFY_NOT_REACHED();
}

Vector<u32> Object::indexed_indices() const
{
    switch (m_indexed_storage_kind) {
    case IndexedStorageKind::None:
        return {};
    case IndexedStorageKind::Packed: {
        Vector<u32> indices;
        indices.ensure_capacity(m_indexed_array_like_size);
        for (u32 i = 0; i < m_indexed_array_like_size; ++i)
            indices.unchecked_append(i);
        return indices;
    }
    case IndexedStorageKind::Holey: {
        Vector<u32> indices;
        auto available_elements = min(m_indexed_array_like_size, indexed_elements_capacity());
        indices.ensure_capacity(available_elements);
        for (u32 i = 0; i < available_elements; ++i) {
            if (!m_indexed_elements[i].is_special_empty_value())
                indices.unchecked_append(i);
        }
        return indices;
    }
    case IndexedStorageKind::Dictionary: {
        auto indices = indexed_dictionary()->sparse_elements().keys();
        quick_sort(indices);
        return indices;
    }
    }
    VERIFY_NOT_REACHED();
}

void Object::set_indexed_property_elements(Vector<Value>&& values)
{
    free_indexed_elements();

    if (values.is_empty())
        return;

    u32 size = values.size();
    m_indexed_storage_kind = IndexedStorageKind::Packed;
    m_indexed_array_like_size = size;
    m_indexed_elements = allocate_indexed_elements(size);
    for (u32 i = 0; i < size; ++i)
        m_indexed_elements[i] = values[i];
}

ReadonlySpan<Value> Object::indexed_packed_elements_span() const
{
    VERIFY(m_indexed_storage_kind == IndexedStorageKind::Packed);
    return { m_indexed_elements, m_indexed_array_like_size };
}

void Object::convert_to_prototype_if_needed()
{
    if (shape().is_prototype_shape())
        return;
    set_shape(shape().clone_for_prototype());
}

}
