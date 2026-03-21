/*
 * Copyright (c) 2025, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/OwnPtr.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/HTML/StructuredSerializeTypes.h>
#include <LibWeb/IndexedDB/Internal/Key.h>

namespace Web::IndexedDB {

// https://w3c.github.io/IndexedDB/#object-store-record
struct ObjectStoreRecord {
    GC::Ref<Key> key;
    OwnPtr<HTML::SerializationRecord> value;

    ObjectStoreRecord(GC::Ref<Key> key, NonnullOwnPtr<HTML::SerializationRecord> value)
        : key(key)
        , value(move(value))
    {
    }

    ObjectStoreRecord(ObjectStoreRecord const& other)
        : key(other.key)
        , value(make<HTML::SerializationRecord>(*other.value))
    {
    }

    ObjectStoreRecord(ObjectStoreRecord&&) = default;
    ObjectStoreRecord& operator=(ObjectStoreRecord&&) = default;
    ObjectStoreRecord& operator=(ObjectStoreRecord const&) = delete;
};

// https://w3c.github.io/IndexedDB/#index-list-of-records
struct IndexRecord {
    GC::Ref<Key> key;
    GC::Ref<Key> value;
};

// https://pr-preview.s3.amazonaws.com/w3c/IndexedDB/pull/461.html#record-snapshot
// https://pr-preview.s3.amazonaws.com/w3c/IndexedDB/461/95f98c0...43e154b.html#record-interface
class IDBRecord : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(IDBRecord, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(IDBRecord);

public:
    [[nodiscard]] static GC::Ref<IDBRecord> create(JS::Realm& realm, GC::Ref<Key> key, JS::Value value, GC::Ref<Key> primary_key);
    virtual ~IDBRecord();

    JS::Value value() const { return m_value; }
    WebIDL::ExceptionOr<JS::Value> key() const;
    WebIDL::ExceptionOr<JS::Value> primary_key() const;

protected:
    explicit IDBRecord(JS::Realm&, GC::Ref<Key> key, JS::Value value, GC::Ref<Key> primary_key);
    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor& visitor) override;

private:
    GC::Ref<Key> m_key;
    JS::Value m_value;
    GC::Ref<Key> m_primary_key;
};

}
