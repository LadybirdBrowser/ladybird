/*
 * Copyright (c) 2025, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <AK/QuickSort.h>
#include <LibWeb/IndexedDB/IDBKeyRange.h>
#include <LibWeb/IndexedDB/Internal/ObjectStore.h>

namespace Web::IndexedDB {

GC_DEFINE_ALLOCATOR(ObjectStore);

ObjectStore::~ObjectStore() = default;

GC::Ref<ObjectStore> ObjectStore::create(JS::Realm& realm, GC::Ref<Database> database, String name, bool auto_increment, Optional<KeyPath> const& key_path)
{
    return realm.create<ObjectStore>(database, name, auto_increment, key_path);
}

ObjectStore::ObjectStore(GC::Ref<Database> database, String name, bool auto_increment, Optional<KeyPath> const& key_path)
    : m_database(database)
    , m_name(move(name))
    , m_key_path(key_path)
{
    database->add_object_store(*this);

    if (auto_increment)
        m_key_generator = KeyGenerator {};
}

void ObjectStore::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_database);
    visitor.visit(m_indexes);

    for (auto& record : m_records) {
        visitor.visit(record.key);
    }
}

void ObjectStore::remove_records_in_range(GC::Ref<IDBKeyRange> range)
{
    m_records.remove_all_matching([&](auto const& record) {
        return range->is_in_range(record.key);
    });
}

bool ObjectStore::has_record_with_key(GC::Ref<Key> key)
{
    auto index = m_records.find_if([&key](auto const& record) {
        return Key::equals(key, record.key);
    });

    return index != m_records.end();
}

void ObjectStore::store_a_record(ObjectStoreRecord const& record)
{
    m_records.append(record);

    // NOTE: The record is stored in the object store’s list of records such that the list is sorted according to the key of the records in ascending order.
    AK::quick_sort(m_records, [](auto const& a, auto const& b) {
        return Key::compare_two_keys(a.key, b.key) < 0;
    });
}

u64 ObjectStore::count_records_in_range(GC::Ref<IDBKeyRange> range)
{
    u64 count = 0;
    for (auto const& record : m_records) {
        if (range->is_in_range(record.key))
            ++count;
    }
    return count;
}

Optional<ObjectStoreRecord&> ObjectStore::first_in_range(GC::Ref<IDBKeyRange> range)
{
    return m_records.first_matching([&](auto const& record) {
        return range->is_in_range(record.key);
    });
}

void ObjectStore::clear_records()
{
    m_records.clear();
}

// https://w3c.github.io/IndexedDB/#generate-a-key
ErrorOr<u64> ObjectStore::generate_a_key()
{
    // 1. Let generator be store's key generator.
    auto& generator = key_generator();

    // 2. Let key be generator's current number.
    auto key = generator.current_number();

    // 3. If key is greater than 2^53 (9007199254740992), then return failure.
    if (key > static_cast<u64>(MAX_KEY_GENERATOR_VALUE))
        return Error::from_string_literal("Key is greater than 2^53 while trying to generate a key");

    // 4. Increase generator's current number by 1.
    generator.increment(1);

    // 5. Return key.
    return key;
}

// https://w3c.github.io/IndexedDB/#possibly-update-the-key-generator
void ObjectStore::possibly_update_the_key_generator(GC::Ref<Key> key)
{
    // 1. If the type of key is not number, abort these steps.
    if (key->type() != Key::KeyType::Number)
        return;

    // 2. Let value be the value of key.
    auto value = key->value_as_double();

    // 3. Set value to the minimum of value and 2^53 (9007199254740992).
    value = min(value, MAX_KEY_GENERATOR_VALUE);

    // 4. Set value to the largest integer not greater than value.
    value = AK::floor(value);

    // 5. Let generator be store's key generator.
    auto& generator = key_generator();

    // 6. If value is greater than or equal to generator's current number, then set generator's current number to value + 1.
    if (value >= static_cast<double>(generator.current_number()))
        generator.set(static_cast<u64>(value + 1));
}

GC::ConservativeVector<ObjectStoreRecord> ObjectStore::first_n_in_range(GC::Ref<IDBKeyRange> range, Optional<WebIDL::UnsignedLong> count)
{
    GC::ConservativeVector<ObjectStoreRecord> records(range->heap());
    for (auto const& record : m_records) {
        if (range->is_in_range(record.key))
            records.append(record);

        if (count.has_value() && records.size() >= *count)
            break;
    }

    return records;
}

GC::ConservativeVector<ObjectStoreRecord> ObjectStore::last_n_in_range(GC::Ref<IDBKeyRange> range, Optional<WebIDL::UnsignedLong> count)
{
    GC::ConservativeVector<ObjectStoreRecord> records(range->heap());
    for (auto const& record : m_records.in_reverse()) {
        if (range->is_in_range(record.key))
            records.append(record);

        if (count.has_value() && records.size() >= *count)
            break;
    }

    return records;
}

}
