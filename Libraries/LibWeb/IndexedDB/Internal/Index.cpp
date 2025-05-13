/*
 * Copyright (c) 2025, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/QuickSort.h>
#include <LibWeb/IndexedDB/Internal/Index.h>
#include <LibWeb/IndexedDB/Internal/ObjectStore.h>

namespace Web::IndexedDB {

GC_DEFINE_ALLOCATOR(Index);

Index::~Index() = default;

GC::Ref<Index> Index::create(JS::Realm& realm, GC::Ref<ObjectStore> store, String const& name, KeyPath const& key_path, bool unique, bool multi_entry)
{
    return realm.create<Index>(store, name, key_path, unique, multi_entry);
}

Index::Index(GC::Ref<ObjectStore> store, String const& name, KeyPath const& key_path, bool unique, bool multi_entry)
    : m_object_store(store)
    , m_name(name)
    , m_unique(unique)
    , m_multi_entry(multi_entry)
    , m_key_path(key_path)
{
    store->index_set().set(name, *this);
}

void Index::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_object_store);

    for (auto& record : m_records) {
        visitor.visit(record.key);
        visitor.visit(record.value);
    }
}

void Index::set_name(String name)
{
    // NOTE: Update the key in the map so it still matches the name
    auto old_value = m_object_store->index_set().take(m_name).release_value();
    m_object_store->index_set().set(name, old_value);

    m_name = move(name);
}

bool Index::has_record_with_key(GC::Ref<Key> key)
{
    auto index = m_records.find_if([&key](auto const& record) {
        return Key::equals(record.key, key);
    });

    return index != m_records.end();
}

// https://w3c.github.io/IndexedDB/#index-referenced-value
HTML::SerializationRecord Index::referenced_value(IndexRecord const& index_record) const
{
    // Records in an index are said to have a referenced value.
    // This is the value of the record in the index’s referenced object store which has a key equal to the index’s record’s value.
    return m_object_store
        ->records()
        .first_matching([&](auto const& store_record) {
            return Key::equals(store_record.key, index_record.value);
        })
        .value()
        .value;
}

void Index::clear_records()
{
    m_records.clear();
}

Optional<IndexRecord&> Index::first_in_range(GC::Ref<IDBKeyRange> range)
{
    return m_records.first_matching([&](auto const& record) {
        return range->is_in_range(record.key);
    });
}

GC::ConservativeVector<IndexRecord> Index::first_n_in_range(GC::Ref<IDBKeyRange> range, Optional<WebIDL::UnsignedLong> count)
{
    GC::ConservativeVector<IndexRecord> records(range->heap());
    for (auto const& record : m_records) {
        if (range->is_in_range(record.key))
            records.append(record);

        if (count.has_value() && records.size() >= *count)
            break;
    }

    return records;
}

u64 Index::count_records_in_range(GC::Ref<IDBKeyRange> range)
{
    u64 count = 0;
    for (auto const& record : m_records) {
        if (range->is_in_range(record.key))
            ++count;
    }
    return count;
}

void Index::store_a_record(IndexRecord const& record)
{
    m_records.append(record);

    // NOTE: The record is stored in index’s list of records such that the list is sorted primarily on the records keys, and secondarily on the records values, in ascending order.
    AK::quick_sort(m_records, [](auto const& a, auto const& b) {
        auto key_comparison = Key::compare_two_keys(a.key, b.key);
        if (key_comparison != 0)
            return key_comparison < 0;

        return Key::compare_two_keys(a.value, b.value) < 0;
    });
}

}
