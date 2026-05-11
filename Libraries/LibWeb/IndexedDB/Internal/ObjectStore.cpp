/*
 * Copyright (c) 2025, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/BinarySearch.h>
#include <AK/Math.h>
#include <LibWeb/IndexedDB/IDBKeyRange.h>
#include <LibWeb/IndexedDB/Internal/MutationLog.h>
#include <LibWeb/IndexedDB/Internal/ObjectStore.h>
#include <LibWeb/IndexedDB/Internal/RecordRange.h>

namespace Web::IndexedDB {

GC_DEFINE_ALLOCATOR(ObjectStore);

ObjectStore::~ObjectStore() = default;

GC::Ref<ObjectStore> ObjectStore::create(JS::Realm& realm, GC::Ref<Database> database, String name, bool auto_increment, Optional<KeyPath> const& key_path)
{
    return realm.create<ObjectStore>(database, name, auto_increment, key_path);
}

size_t ObjectStore::mutation_log_position() const
{
    if (!m_mutation_log)
        return 0;
    return m_mutation_log->position();
}

void ObjectStore::revert_mutations_from(size_t position)
{
    if (m_mutation_log)
        m_mutation_log->revert_from(*this, position);
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
    visitor.visit(m_mutation_log);

    for (auto& record : m_records) {
        visitor.visit(record.key);
    }
}

void ObjectStore::remove_records_in_range(GC::Ref<IDBKeyRange> range)
{
    if (m_records.is_empty())
        return;

    // Since records are sorted by key, records in range form a contiguous block.
    auto record_range = record_range_for_key_range(m_records, range);

    if (record_range.start < record_range.end) {
        if (m_mutation_log) {
            Vector<ObjectStoreRecord> deleted;
            deleted.ensure_capacity(record_range.end - record_range.start);
            for (size_t i = record_range.start; i < record_range.end; ++i)
                deleted.append(move(m_records[i]));
            m_mutation_log->note_records_deleted(move(deleted));
        }
        m_records.remove(record_range.start, record_range.end - record_range.start);
    }
}

void ObjectStore::remove_record_with_key(GC::Ref<Key> key)
{
    size_t index = 0;
    auto* record = AK::binary_search(m_records, key, &index, [](auto const& needle, auto const& record) {
        return Key::compare_two_keys(needle, record.key);
    });
    if (record)
        m_records.remove(index);
}

bool ObjectStore::has_record_with_key(GC::Ref<Key> key)
{
    return binary_search(m_records, key, nullptr, [](auto const& needle, auto const& record) -> int {
        return Key::compare_two_keys(needle, record.key);
    }) != nullptr;
}

void ObjectStore::store_a_record(ObjectStoreRecord record)
{
    if (m_mutation_log)
        m_mutation_log->note_record_stored(record.key);

    // NOTE: The record is stored in the object store’s list of records such that the list is sorted according to the key of the records in ascending order.
    if (m_records.is_empty() || Key::compare_two_keys(m_records.last().key, record.key) <= 0) {
        m_records.append(move(record));
        return;
    }

    m_records.insert(first_record_index_with_key_at_or_after(m_records, record.key, false), move(record));
}

u64 ObjectStore::count_records_in_range(GC::Ref<IDBKeyRange> range)
{
    auto record_range = record_range_for_key_range(m_records, range);
    return record_range.end - record_range.start;
}

Optional<ObjectStoreRecord&> ObjectStore::first_in_range(GC::Ref<IDBKeyRange> range)
{
    auto record_range = record_range_for_key_range(m_records, range);
    if (record_range.start == record_range.end)
        return {};
    return m_records[record_range.start];
}

void ObjectStore::clear_records()
{
    auto deleted_records = move(m_records);
    if (m_mutation_log && !deleted_records.is_empty())
        m_mutation_log->note_records_deleted(deleted_records);
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
    if (m_mutation_log)
        m_mutation_log->note_key_generator_changed(key);
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
    if (value >= static_cast<double>(generator.current_number())) {
        if (m_mutation_log)
            m_mutation_log->note_key_generator_changed(generator.current_number());
        generator.set(static_cast<u64>(value + 1));
    }
}

GC::ConservativeVector<ObjectStoreRecord> ObjectStore::first_n_in_range(GC::Ref<IDBKeyRange> range, Optional<WebIDL::UnsignedLong> count)
{
    GC::ConservativeVector<ObjectStoreRecord> records(range->heap());
    auto record_range = record_range_for_key_range(m_records, range);
    for (size_t i = record_range.start; i < record_range.end; ++i) {
        records.append(m_records[i]);

        if (count.has_value() && records.size() >= *count)
            break;
    }

    return records;
}

GC::ConservativeVector<ObjectStoreRecord> ObjectStore::last_n_in_range(GC::Ref<IDBKeyRange> range, Optional<WebIDL::UnsignedLong> count)
{
    GC::ConservativeVector<ObjectStoreRecord> records(range->heap());
    auto record_range = record_range_for_key_range(m_records, range);
    for (size_t i = record_range.end; i > record_range.start;) {
        --i;
        records.append(m_records[i]);

        if (count.has_value() && records.size() >= *count)
            break;
    }

    return records;
}

}
