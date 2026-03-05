/*
 * Copyright (c) 2025, zaggy1024
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibGC/Ptr.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/IndexedDB/IDBRecord.h>
#include <LibWeb/IndexedDB/Internal/KeyGenerator.h>

namespace Web::IndexedDB {

class Database;
class IDBDatabase;
class Index;
class ObjectStore;

// Tracks mutations to an object store and its indexes so they can be reverted on request failure
// (asynchronously execute a request step 5.4) or transaction abort (abort a transaction step 2).
//
// For upgrade transactions, this also tracks schema-level changes (store/index creation and deletion).
// Schema entries are always at the boundaries: ObjectStoreCreated is always first (if present),
// and ObjectStoreDeleted is always last (if present). Index schema entries appear in order between
// data mutations.
class MutationLog : public JS::Cell {
    GC_CELL(MutationLog, JS::Cell);
    GC_DECLARE_ALLOCATOR(MutationLog);

public:
    [[nodiscard]] static GC::Ref<MutationLog> create(JS::Realm&);

    // Schema-level entries (upgrade transactions only).
    void note_object_store_created();
    void note_object_store_deleted();
    void note_object_store_renamed(String old_name);
    void note_index_created(GC::Ref<Index>);
    void note_index_deleted(GC::Ref<Index>);
    void note_index_renamed(GC::Ref<Index>, String old_name);

    // Record that the key generator value was changed, saving the old value for revert.
    void note_key_generator_changed(u64 old_value);

    // Record that records were deleted from the object store, saving them for re-insertion on revert.
    void note_records_deleted(Vector<ObjectStoreRecord>);

    // Record that a new record was stored in the object store, saving the key for deletion on revert.
    void note_record_stored(GC::Ref<Key> key);

    // Record that records were deleted from an index, saving them for re-insertion on revert.
    void note_index_records_deleted(GC::Ref<Index>, Vector<IndexRecord>);

    // Record that a new record was stored in an index, saving it for deletion on revert.
    void note_index_record_stored(GC::Ref<Index>, IndexRecord);

    // Undo all logged mutations in reverse order. Database and connection are required for schema entries.
    void revert(ObjectStore&, GC::Ref<Database>, GC::Ref<IDBDatabase>);

    // Undo logged mutations from the given position forward (used for per-request revert on failure).
    // Asserts that no schema entries are encountered.
    void revert_from(ObjectStore&, size_t position);

    // Clear the log without reverting (used after successful transaction commit).
    void clear() { m_entries.clear(); }

    [[nodiscard]] size_t position() const { return m_entries.size(); }

protected:
    explicit MutationLog();
    virtual void visit_edges(Visitor&) override;

private:
    void revert_entries(ObjectStore&, size_t from_position, GC::Ptr<Database>, GC::Ptr<IDBDatabase>);

    struct ObjectStoreCreated { };
    struct ObjectStoreDeleted { };

    struct IndexCreated {
        GC::Ref<Index> index;
    };

    struct IndexDeleted {
        GC::Ref<Index> index;
    };

    struct ObjectStoreRenamed {
        String old_name;
    };

    struct IndexRenamed {
        GC::Ref<Index> index;
        String old_name;
    };

    struct KeyGeneratorChanged {
        u64 old_value;
    };

    struct RecordsDeleted {
        Vector<ObjectStoreRecord> records;
    };

    struct RecordStored {
        GC::Ref<Key> key;
    };

    struct IndexRecordsDeleted {
        GC::Ref<Index> index;
        Vector<IndexRecord> records;
    };

    struct IndexRecordStored {
        GC::Ref<Index> index;
        IndexRecord record;
    };

    using Entry = Variant<ObjectStoreCreated, ObjectStoreDeleted, ObjectStoreRenamed, IndexCreated, IndexDeleted, IndexRenamed, KeyGeneratorChanged, RecordsDeleted, RecordStored, IndexRecordsDeleted, IndexRecordStored>;

    Vector<Entry> m_entries;
};

}
