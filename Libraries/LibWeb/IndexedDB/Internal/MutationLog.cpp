/*
 * Copyright (c) 2025, zaggy1024
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/IndexedDB/IDBDatabase.h>
#include <LibWeb/IndexedDB/Internal/Database.h>
#include <LibWeb/IndexedDB/Internal/Index.h>
#include <LibWeb/IndexedDB/Internal/MutationLog.h>
#include <LibWeb/IndexedDB/Internal/ObjectStore.h>

namespace Web::IndexedDB {

GC_DEFINE_ALLOCATOR(MutationLog);

MutationLog::MutationLog() = default;

GC::Ref<MutationLog> MutationLog::create(JS::Realm& realm)
{
    return realm.create<MutationLog>();
}

void MutationLog::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    for (auto& entry : m_entries) {
        entry.visit(
            [&](ObjectStoreCreated&) {
            },
            [&](ObjectStoreDeleted&) {
            },
            [&](IndexCreated& e) {
                visitor.visit(e.index);
            },
            [&](IndexDeleted& e) {
                visitor.visit(e.index);
            },
            [&](ObjectStoreRenamed&) {
            },
            [&](IndexRenamed& e) {
                visitor.visit(e.index);
            },
            [&](KeyGeneratorChanged&) {
            },
            [&](RecordsDeleted& e) {
                for (auto& record : e.records)
                    visitor.visit(record.key);
            },
            [&](RecordStored& e) {
                visitor.visit(e.key);
            },
            [&](IndexRecordsDeleted& e) {
                visitor.visit(e.index);
                for (auto& record : e.records) {
                    visitor.visit(record.key);
                    visitor.visit(record.value);
                }
            },
            [&](IndexRecordStored& e) {
                visitor.visit(e.index);
                visitor.visit(e.record.key);
                visitor.visit(e.record.value);
            });
    }
}

void MutationLog::note_object_store_created()
{
    m_entries.append(ObjectStoreCreated {});
}

void MutationLog::note_object_store_deleted()
{
    m_entries.append(ObjectStoreDeleted {});
}

void MutationLog::note_object_store_renamed(String old_name)
{
    m_entries.append(ObjectStoreRenamed { move(old_name) });
}

void MutationLog::note_index_created(GC::Ref<Index> index)
{
    m_entries.append(IndexCreated { index });
}

void MutationLog::note_index_deleted(GC::Ref<Index> index)
{
    m_entries.append(IndexDeleted { index });
}

void MutationLog::note_index_renamed(GC::Ref<Index> index, String old_name)
{
    m_entries.append(IndexRenamed { index, move(old_name) });
}

void MutationLog::note_key_generator_changed(u64 old_value)
{
    m_entries.append(KeyGeneratorChanged { old_value });
}

void MutationLog::note_records_deleted(Vector<ObjectStoreRecord> records)
{
    m_entries.append(RecordsDeleted { move(records) });
}

void MutationLog::note_record_stored(GC::Ref<Key> key)
{
    m_entries.append(RecordStored { key });
}

void MutationLog::note_index_records_deleted(GC::Ref<Index> index, Vector<IndexRecord> records)
{
    m_entries.append(IndexRecordsDeleted { index, move(records) });
}

void MutationLog::note_index_record_stored(GC::Ref<Index> index, IndexRecord record)
{
    m_entries.append(IndexRecordStored { index, record });
}

void MutationLog::revert(ObjectStore& store, GC::Ref<Database> database, GC::Ref<IDBDatabase> connection)
{
    revert_entries(store, 0, database, connection);
}

void MutationLog::revert_from(ObjectStore& store, size_t position)
{
    revert_entries(store, position, nullptr, nullptr);
}

void MutationLog::revert_entries(ObjectStore& store, size_t from_position, GC::Ptr<Database> database, GC::Ptr<IDBDatabase> connection)
{
    // Temporarily clear the store's mutation log pointer so that the operations below
    // don't re-log into this log while we're reverting it.
    auto saved_log = store.mutation_log();
    store.set_mutation_log(nullptr);

    for (size_t i = m_entries.size(); i-- > from_position;) {
        m_entries[i].visit(
            [&](ObjectStoreCreated&) {
                VERIFY(database);
                VERIFY(connection);
                database->remove_object_store(store);
                connection->remove_from_object_store_set(store);
            },
            [&](ObjectStoreDeleted&) {
                VERIFY(database);
                VERIFY(connection);
                store.set_deleted(false);
                for (auto const& [_, index] : store.index_set())
                    index->set_deleted(false);
                database->add_object_store(store);
                connection->add_to_object_store_set(store);
            },
            [&](IndexCreated& e) {
                VERIFY(database);
                e.index->object_store()->index_set().remove(e.index->name());
            },
            [&](IndexDeleted& e) {
                VERIFY(database);
                e.index->set_deleted(false);
                e.index->object_store()->index_set().set(e.index->name(), e.index);
            },
            [&](ObjectStoreRenamed& e) {
                store.set_name(e.old_name);
            },
            [&](IndexRenamed& e) {
                e.index->set_name(move(e.old_name));
            },
            [&](KeyGeneratorChanged& e) {
                store.key_generator().set(e.old_value);
            },
            [&](RecordsDeleted& e) {
                for (auto& record : e.records)
                    store.store_a_record(record);
            },
            [&](RecordStored& e) {
                store.remove_record_with_key(e.key);
            },
            [&](IndexRecordsDeleted& e) {
                for (auto& record : e.records)
                    e.index->store_a_record(record);
            },
            [&](IndexRecordStored& e) {
                e.index->remove_record(e.record);
            });
    }
    m_entries.shrink(from_position);

    store.set_mutation_log(saved_log);
}

}
