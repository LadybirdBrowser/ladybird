/*
 * Copyright (c) 2025, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/QuickSort.h>
#include <LibWeb/IndexedDB/IDBKeyRange.h>
#include <LibWeb/IndexedDB/Internal/ObjectStore.h>

namespace Web::IndexedDB {

GC_DEFINE_ALLOCATOR(ObjectStore);

ObjectStore::~ObjectStore() = default;

GC::Ref<ObjectStore> ObjectStore::create(JS::Realm& realm, GC::Ref<Database> database, String const& name, bool auto_increment, Optional<KeyPath> const& key_path)
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
