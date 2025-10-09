/*
 * Copyright (c) 2025, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibGC/Ptr.h>
#include <LibJS/Heap/Cell.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/IndexedDB/IDBRecord.h>
#include <LibWeb/IndexedDB/Internal/Algorithms.h>
#include <LibWeb/IndexedDB/Internal/Database.h>
#include <LibWeb/IndexedDB/Internal/Index.h>
#include <LibWeb/IndexedDB/Internal/KeyGenerator.h>

namespace Web::IndexedDB {

using KeyPath = Variant<String, Vector<String>>;

// https://w3c.github.io/IndexedDB/#object-store-construct
class ObjectStore : public JS::Cell {
    GC_CELL(ObjectStore, JS::Cell);
    GC_DECLARE_ALLOCATOR(ObjectStore);

public:
    [[nodiscard]] static GC::Ref<ObjectStore> create(JS::Realm&, GC::Ref<Database>, String const&, bool, Optional<KeyPath> const&);
    virtual ~ObjectStore();

    String name() const { return m_name; }
    void set_name(String name) { m_name = move(name); }
    Optional<KeyPath> key_path() const { return m_key_path; }
    bool uses_inline_keys() const { return m_key_path.has_value(); }
    bool uses_out_of_line_keys() const { return !m_key_path.has_value(); }
    KeyGenerator& key_generator() { return *m_key_generator; }
    bool uses_a_key_generator() const { return m_key_generator.has_value(); }
    AK::HashMap<String, GC::Ref<Index>>& index_set() { return m_indexes; }

    GC::Ref<Database> database() const { return m_database; }
    ReadonlySpan<ObjectStoreRecord> records() const { return m_records; }

    void remove_records_in_range(GC::Ref<IDBKeyRange> range);
    bool has_record_with_key(GC::Ref<Key> key);
    void store_a_record(ObjectStoreRecord const& record);
    u64 count_records_in_range(GC::Ref<IDBKeyRange> range);
    Optional<ObjectStoreRecord&> first_in_range(GC::Ref<IDBKeyRange> range);
    void clear_records();
    GC::ConservativeVector<ObjectStoreRecord> first_n_in_range(GC::Ref<IDBKeyRange> range, Optional<WebIDL::UnsignedLong> count);
    GC::ConservativeVector<ObjectStoreRecord> last_n_in_range(GC::Ref<IDBKeyRange> range, Optional<WebIDL::UnsignedLong> count);

protected:
    virtual void visit_edges(Visitor&) override;

private:
    ObjectStore(GC::Ref<Database> database, String name, bool auto_increment, Optional<KeyPath> const& key_path);

    // AD-HOC: An ObjectStore needs to know what Database it belongs to...
    GC::Ref<Database> m_database;

    // AD-HOC: An Index has referenced ObjectStores, we also need the reverse mapping
    AK::HashMap<String, GC::Ref<Index>> m_indexes;

    // An object store has a name, which is a name. At any one time, the name is unique within the database to which it belongs.
    String m_name;

    // An object store optionally has a key path. If the object store has a key path it is said to use in-line keys. Otherwise it is said to use out-of-line keys.
    Optional<KeyPath> m_key_path;

    // An object store optionally has a key generator.
    Optional<KeyGenerator> m_key_generator;

    // An object store has a list of records
    Vector<ObjectStoreRecord> m_records;
};

}
