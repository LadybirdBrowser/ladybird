/*
 * Copyright (c) 2024, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <LibGC/Heap.h>
#include <LibGC/Ptr.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/IndexedDB/Internal/Key.h>

namespace Web::IndexedDB {

// https://w3c.github.io/IndexedDB/#keyrange
class IDBKeyRange : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(IDBKeyRange, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(IDBKeyRange);

    enum class LowerOpen {
        No,
        Yes,
    };

    enum class UpperOpen {
        No,
        Yes,
    };

public:
    virtual ~IDBKeyRange() override;
    [[nodiscard]] static GC::Ref<IDBKeyRange> create(JS::Realm&, GC::Ptr<Key> lower_bound, GC::Ptr<Key> upper_bound, LowerOpen lower_open, UpperOpen upper_open);

    [[nodiscard]] JS::Value lower() const;
    [[nodiscard]] JS::Value upper() const;
    bool lower_open() const { return m_lower_open; }
    bool upper_open() const { return m_upper_open; }

    static WebIDL::ExceptionOr<GC::Ref<IDBKeyRange>> only(JS::VM&, JS::Value);
    static WebIDL::ExceptionOr<GC::Ref<IDBKeyRange>> lower_bound(JS::VM&, JS::Value, bool);
    static WebIDL::ExceptionOr<GC::Ref<IDBKeyRange>> upper_bound(JS::VM&, JS::Value, bool);
    static WebIDL::ExceptionOr<GC::Ref<IDBKeyRange>> bound(JS::VM&, JS::Value, JS::Value, bool, bool);
    WebIDL::ExceptionOr<bool> includes(JS::Value);

    bool is_unbound() const { return m_lower_bound == nullptr && m_upper_bound == nullptr; }
    bool is_in_range(GC::Ref<Key>) const;
    GC::Ptr<Key> lower_key() const { return m_lower_bound; }
    GC::Ptr<Key> upper_key() const { return m_upper_bound; }

protected:
    explicit IDBKeyRange(JS::Realm&, GC::Ptr<Key> lower_bound, GC::Ptr<Key> upper_bound, LowerOpen lower_open, UpperOpen upper_open);
    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor& visitor) override;

private:
    // A key range has an associated lower bound (null or a key).
    GC::Ptr<Key> m_lower_bound;

    // A key range has an associated upper bound (null or a key).
    GC::Ptr<Key> m_upper_bound;

    // A key range has an associated lower open flag. Unless otherwise stated it is false.
    bool m_lower_open { false };

    // A key range has an associated upper open flag. Unless otherwise stated it is false.
    bool m_upper_open { false };
};

}
