/*
 * Copyright (c) 2025, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibGC/Ptr.h>
#include <LibJS/Heap/Cell.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/IndexedDB/Internal/ObjectStore.h>

namespace Web::IndexedDB {

using KeyPath = Variant<String, Vector<String>>;

// https://w3c.github.io/IndexedDB/#index-list-of-records
struct IndexRecord {
    GC::Ref<Key> key;
    GC::Ref<Key> value;
};

// https://w3c.github.io/IndexedDB/#index-construct
class Index : public JS::Cell {
    GC_CELL(Index, JS::Cell);
    GC_DECLARE_ALLOCATOR(Index);

public:
    [[nodiscard]] static GC::Ref<Index> create(JS::Realm&, GC::Ref<ObjectStore>, String const&, KeyPath const&, bool, bool);
    virtual ~Index();

    void set_name(String name);
    [[nodiscard]] String name() const { return m_name; }
    [[nodiscard]] bool unique() const { return m_unique; }
    [[nodiscard]] bool multi_entry() const { return m_multi_entry; }
    [[nodiscard]] GC::Ref<ObjectStore> object_store() const { return m_object_store; }
    [[nodiscard]] AK::ReadonlySpan<IndexRecord> records() const { return m_records; }
    [[nodiscard]] KeyPath const& key_path() const { return m_key_path; }

    [[nodiscard]] bool has_record_with_key(GC::Ref<Key> key);

    HTML::SerializationRecord referenced_value(IndexRecord const& index_record) const;

protected:
    virtual void visit_edges(Visitor&) override;

private:
    Index(GC::Ref<ObjectStore>, String const&, KeyPath const&, bool, bool);

    // An index [...] has a referenced object store.
    GC::Ref<ObjectStore> m_object_store;

    // The index has a list of records which hold the data stored in the index.
    Vector<IndexRecord> m_records;

    // An index has a name, which is a name. At any one time, the name is unique within index’s referenced object store.
    String m_name;

    // An index has a unique flag. When true, the index enforces that no two records in the index has the same key.
    bool m_unique { false };

    // An index has a multiEntry flag. This flag affects how the index behaves when the result of evaluating the index’s key path yields an array key.
    bool m_multi_entry { false };

    // The keys are derived from the referenced object store’s values using a key path.
    KeyPath m_key_path;
};

}
