/*
 * Copyright (c) 2024, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Optional.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/Bindings/IDBKeyRangePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/IndexedDB/IDBKeyRange.h>
#include <LibWeb/IndexedDB/Internal/Algorithms.h>

namespace Web::IndexedDB {

GC_DEFINE_ALLOCATOR(IDBKeyRange);

IDBKeyRange::~IDBKeyRange() = default;

IDBKeyRange::IDBKeyRange(JS::Realm& realm, GC::Ptr<Key> lower_bound, GC::Ptr<Key> upper_bound, bool lower_open, bool upper_open)
    : PlatformObject(realm)
    , m_lower_bound(lower_bound)
    , m_upper_bound(upper_bound)
    , m_lower_open(lower_open)
    , m_upper_open(upper_open)
{
}

GC::Ref<IDBKeyRange> IDBKeyRange::create(JS::Realm& realm, GC::Ptr<Key> lower_bound, GC::Ptr<Key> upper_bound, bool lower_open, bool upper_open)
{
    return realm.create<IDBKeyRange>(realm, lower_bound, upper_bound, lower_open, upper_open);
}

void IDBKeyRange::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(IDBKeyRange);
    Base::initialize(realm);
}

void IDBKeyRange::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_lower_bound);
    visitor.visit(m_upper_bound);
}

// https://w3c.github.io/IndexedDB/#in
bool IDBKeyRange::is_in_range(GC::Ref<Key> key) const
{
    // A key is in a key range range if both of the following conditions are fulfilled:

    // The range’s lower bound is null, or it is less than key, or it is both equal to key and the range’s lower open flag is false.
    auto lower_bound_in_range = this->lower_key() == nullptr || Key::less_than(*this->lower_key(), key) || (Key::equals(key, *this->lower_key()) && !this->lower_open());

    // The range’s upper bound is null, or it is greater than key, or it is both equal to key and the range’s upper open flag is false.
    auto upper_bound_in_range = this->upper_key() == nullptr || Key::greater_than(*this->upper_key(), key) || (Key::equals(key, *this->upper_key()) && !this->upper_open());

    return lower_bound_in_range && upper_bound_in_range;
}

// https://w3c.github.io/IndexedDB/#dom-idbkeyrange-only
WebIDL::ExceptionOr<GC::Ref<IDBKeyRange>> IDBKeyRange::only(JS::VM& vm, JS::Value value)
{
    auto& realm = *vm.current_realm();

    // 1. Let key be the result of converting a value to a key with value. Rethrow any exceptions.
    auto key = TRY(convert_a_value_to_a_key(realm, value));

    // 2. If key is invalid, throw a "DataError" DOMException.
    if (key->is_invalid())
        return WebIDL::DataError::create(realm, "Value is invalid"_string);

    // 3. Create and return a new key range containing only key.
    return IDBKeyRange::create(realm, key, key, false, false);
}

// https://w3c.github.io/IndexedDB/#dom-idbkeyrange-lowerbound
WebIDL::ExceptionOr<GC::Ref<IDBKeyRange>> IDBKeyRange::lower_bound(JS::VM& vm, JS::Value lower, bool open)
{
    auto& realm = *vm.current_realm();

    // 1. Let lowerKey be the result of converting a value to a key with lower. Rethrow any exceptions.
    auto key = TRY(convert_a_value_to_a_key(realm, lower));

    // 2. If lowerKey is invalid, throw a "DataError" DOMException.
    if (key->is_invalid())
        return WebIDL::DataError::create(realm, "Value is invalid"_string);

    // 3. Create and return a new key range with lower bound set to lowerKey, lower open flag set to open, upper bound set to null, and upper open flag set to true.
    return IDBKeyRange::create(realm, key, {}, open, true);
}

// https://w3c.github.io/IndexedDB/#dom-idbkeyrange-upperbound
WebIDL::ExceptionOr<GC::Ref<IDBKeyRange>> IDBKeyRange::upper_bound(JS::VM& vm, JS::Value upper, bool open)
{
    auto& realm = *vm.current_realm();

    // 1. Let upperKey be the result of converting a value to a key with upper. Rethrow any exceptions.
    auto key = TRY(convert_a_value_to_a_key(realm, upper));

    // 2. If upperKey is invalid, throw a "DataError" DOMException.
    if (key->is_invalid())
        return WebIDL::DataError::create(realm, "Value is invalid"_string);

    // 3. Create and return a new key range with lower bound set to null, lower open flag set to true, upper bound set to upperKey, and upper open flag set to open.
    return IDBKeyRange::create(realm, {}, key, true, open);
}

// https://w3c.github.io/IndexedDB/#dom-idbkeyrange-bound
WebIDL::ExceptionOr<GC::Ref<IDBKeyRange>> IDBKeyRange::bound(JS::VM& vm, JS::Value lower, JS::Value upper, bool lower_open, bool upper_open)
{
    auto& realm = *vm.current_realm();

    // 1. Let lowerKey be the result of converting a value to a key with lower. Rethrow any exceptions.
    auto lower_key = TRY(convert_a_value_to_a_key(realm, lower));

    // 2. If lowerKey is invalid, throw a "DataError" DOMException.
    if (lower_key->is_invalid())
        return WebIDL::DataError::create(realm, "Value is invalid"_string);

    // 3. Let upperKey be the result of converting a value to a key with upper. Rethrow any exceptions.
    auto upper_key = TRY(convert_a_value_to_a_key(realm, upper));

    // 4. If upperKey is invalid, throw a "DataError" DOMException.
    if (upper_key->is_invalid())
        return WebIDL::DataError::create(realm, "Value is invalid"_string);

    // 5. If lowerKey is greater than upperKey, throw a "DataError" DOMException.
    if (Key::less_than(upper_key, lower_key))
        return WebIDL::DataError::create(realm, "Lower key is greater than upper key"_string);

    // 6. Create and return a new key range with lower bound set to lowerKey, lower open flag set to lowerOpen, upper bound set to upperKey and upper open flag set to upperOpen.
    return IDBKeyRange::create(realm, lower_key, upper_key, lower_open, upper_open);
}

// https://w3c.github.io/IndexedDB/#dom-idbkeyrange-includes
WebIDL::ExceptionOr<bool> IDBKeyRange::includes(JS::Value key)
{
    auto& realm = this->realm();

    // 1. Let k be the result of converting a value to a key with key. Rethrow any exceptions.
    auto k = TRY(convert_a_value_to_a_key(realm, key));

    // 2. If k is invalid, throw a "DataError" DOMException.
    if (k->is_invalid())
        return WebIDL::DataError::create(realm, "Value is invalid"_string);

    // 3. Return true if k is in this range, and false otherwise.
    return is_in_range(k);
}

// https://w3c.github.io/IndexedDB/#dom-idbkeyrange-lower
JS::Value IDBKeyRange::lower() const
{
    // The lower getter steps are to return the result of converting a key to a value with this's lower bound if it is not null, or undefined otherwise.
    if (m_lower_bound)
        return convert_a_key_to_a_value(this->realm(), *m_lower_bound);

    return JS::js_undefined();
}

// https://w3c.github.io/IndexedDB/#dom-idbkeyrange-upper
JS::Value IDBKeyRange::upper() const
{
    // The upper getter steps are to return the result of converting a key to a value with this's upper bound if it is not null, or undefined otherwise.
    if (m_upper_bound)
        return convert_a_key_to_a_value(this->realm(), *m_upper_bound);

    return JS::js_undefined();
}

}
