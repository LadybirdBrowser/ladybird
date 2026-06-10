/*
 * Copyright (c) 2024, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Optional.h>
#include <LibGC/Heap.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/Bindings/ImplementedInBindings.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/IndexedDB/IDBKeyRange.h>
#include <LibWeb/IndexedDB/Internal/Algorithms.h>

namespace Web::IndexedDB {

GC_DEFINE_ALLOCATOR(IDBKeyRange);

IDBKeyRange::~IDBKeyRange() = default;

IDBKeyRange::IDBKeyRange(GC::Ptr<Key> lower_bound, GC::Ptr<Key> upper_bound, LowerOpen lower_open, UpperOpen upper_open)
    : m_lower_bound(lower_bound)
    , m_upper_bound(upper_bound)
    , m_lower_open(lower_open == LowerOpen::Yes)
    , m_upper_open(upper_open == UpperOpen::Yes)
{
}

GC::Ref<IDBKeyRange> IDBKeyRange::create(GC::Ptr<Key> lower_bound, GC::Ptr<Key> upper_bound, LowerOpen lower_open, UpperOpen upper_open)
{
    return GC::Heap::the().allocate<IDBKeyRange>(lower_bound, upper_bound, lower_open, upper_open);
}

void IDBKeyRange::visit_edges(GC::Cell::Visitor& visitor)
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

}

namespace Web::Bindings {

IndexedDB::IDBKeyRange* key_range_from_value(JS::Value value)
{
    if (!value.is_object())
        return nullptr;
    return Bindings::impl_from<IndexedDB::IDBKeyRange>(&value.as_object());
}

// https://w3c.github.io/IndexedDB/#dom-idbkeyrange-only
WebIDL::ExceptionOr<GC::Ref<IndexedDB::IDBKeyRange>> only(JS::Realm& realm, JS::Value value)
{
    // 1. Let key be the result of converting a value to a key with value. Rethrow any exceptions.
    auto key = TRY(IndexedDB::convert_a_value_to_a_key(realm, value));

    // 2. If key is invalid, throw a "DataError" DOMException.
    if (key->is_invalid())
        return WebIDL::DataError::create(realm, "Value is invalid"_utf16);

    // 3. Create and return a new key range containing only key.
    return IndexedDB::IDBKeyRange::create(key, key, IndexedDB::IDBKeyRange::LowerOpen::No, IndexedDB::IDBKeyRange::UpperOpen::No);
}

// https://w3c.github.io/IndexedDB/#dom-idbkeyrange-lowerbound
WebIDL::ExceptionOr<GC::Ref<IndexedDB::IDBKeyRange>> lower_bound(JS::Realm& realm, JS::Value lower, bool open)
{
    // 1. Let lowerKey be the result of converting a value to a key with lower. Rethrow any exceptions.
    auto key = TRY(IndexedDB::convert_a_value_to_a_key(realm, lower));

    // 2. If lowerKey is invalid, throw a "DataError" DOMException.
    if (key->is_invalid())
        return WebIDL::DataError::create(realm, "Value is invalid"_utf16);

    // 3. Create and return a new key range with lower bound set to lowerKey, lower open flag set to open, upper bound set to null, and upper open flag set to true.
    return IndexedDB::IDBKeyRange::create(key, {}, open ? IndexedDB::IDBKeyRange::LowerOpen::Yes : IndexedDB::IDBKeyRange::LowerOpen::No, IndexedDB::IDBKeyRange::UpperOpen::Yes);
}

// https://w3c.github.io/IndexedDB/#dom-idbkeyrange-upperbound
WebIDL::ExceptionOr<GC::Ref<IndexedDB::IDBKeyRange>> upper_bound(JS::Realm& realm, JS::Value upper, bool open)
{
    // 1. Let upperKey be the result of converting a value to a key with upper. Rethrow any exceptions.
    auto key = TRY(IndexedDB::convert_a_value_to_a_key(realm, upper));

    // 2. If upperKey is invalid, throw a "DataError" DOMException.
    if (key->is_invalid())
        return WebIDL::DataError::create(realm, "Value is invalid"_utf16);

    // 3. Create and return a new key range with lower bound set to null, lower open flag set to true, upper bound set to upperKey, and upper open flag set to open.
    return IndexedDB::IDBKeyRange::create({}, key, IndexedDB::IDBKeyRange::LowerOpen::Yes, open ? IndexedDB::IDBKeyRange::UpperOpen::Yes : IndexedDB::IDBKeyRange::UpperOpen::No);
}

// https://w3c.github.io/IndexedDB/#dom-idbkeyrange-bound
WebIDL::ExceptionOr<GC::Ref<IndexedDB::IDBKeyRange>> bound(JS::Realm& realm, JS::Value lower, JS::Value upper, bool lower_open, bool upper_open)
{
    // 1. Let lowerKey be the result of converting a value to a key with lower. Rethrow any exceptions.
    auto lower_key = TRY(IndexedDB::convert_a_value_to_a_key(realm, lower));

    // 2. If lowerKey is invalid, throw a "DataError" DOMException.
    if (lower_key->is_invalid())
        return WebIDL::DataError::create(realm, "Value is invalid"_utf16);

    // 3. Let upperKey be the result of converting a value to a key with upper. Rethrow any exceptions.
    auto upper_key = TRY(IndexedDB::convert_a_value_to_a_key(realm, upper));

    // 4. If upperKey is invalid, throw a "DataError" DOMException.
    if (upper_key->is_invalid())
        return WebIDL::DataError::create(realm, "Value is invalid"_utf16);

    // 5. If lowerKey is greater than upperKey, throw a "DataError" DOMException.
    if (IndexedDB::Key::less_than(upper_key, lower_key))
        return WebIDL::DataError::create(realm, "Lower key is greater than upper key"_utf16);

    // 6. Create and return a new key range with lower bound set to lowerKey, lower open flag set to lowerOpen, upper bound set to upperKey and upper open flag set to upperOpen.
    return IndexedDB::IDBKeyRange::create(lower_key, upper_key, lower_open ? IndexedDB::IDBKeyRange::LowerOpen::Yes : IndexedDB::IDBKeyRange::LowerOpen::No, upper_open ? IndexedDB::IDBKeyRange::UpperOpen::Yes : IndexedDB::IDBKeyRange::UpperOpen::No);
}

// https://w3c.github.io/IndexedDB/#dom-idbkeyrange-includes
WebIDL::ExceptionOr<bool> includes(JS::Realm& realm, IndexedDB::IDBKeyRange& key_range, JS::Value key)
{
    // 1. Let k be the result of converting a value to a key with key. Rethrow any exceptions.
    auto converted_key = TRY(IndexedDB::convert_a_value_to_a_key(realm, key));

    // 2. If k is invalid, throw a "DataError" DOMException.
    if (converted_key->is_invalid())
        return WebIDL::DataError::create(realm, "Value is invalid"_utf16);

    // 3. Return true if k is in this range, and false otherwise.
    return key_range.is_in_range(converted_key);
}

JS::Value key_range_lower(JS::Realm& realm, IndexedDB::IDBKeyRange& key_range)
{
    // The lower getter steps are to return the result of converting a key to a
    // value with this's lower bound if it is not null, or undefined otherwise.
    if (auto lower_key = key_range.lower_key())
        return IndexedDB::convert_a_key_to_a_value(realm, *lower_key);

    return JS::js_undefined();
}

JS::Value key_range_upper(JS::Realm& realm, IndexedDB::IDBKeyRange& key_range)
{
    // The upper getter steps are to return the result of converting a key to a
    // value with this's upper bound if it is not null, or undefined otherwise.
    if (auto upper_key = key_range.upper_key())
        return IndexedDB::convert_a_key_to_a_value(realm, *upper_key);

    return JS::js_undefined();
}

}
