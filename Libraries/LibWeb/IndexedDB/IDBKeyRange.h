/*
 * Copyright (c) 2024, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <LibGC/Heap.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/IndexedDB/Internal/Key.h>

namespace Web::IndexedDB {

// https://w3c.github.io/IndexedDB/#keyrange
class IDBKeyRange : public Bindings::Wrappable {
    WEB_WRAPPABLE(IDBKeyRange, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(IDBKeyRange);

public:
    enum class LowerOpen {
        No,
        Yes,
    };

    enum class UpperOpen {
        No,
        Yes,
    };

    virtual ~IDBKeyRange() override;
    [[nodiscard]] static GC::Ref<IDBKeyRange> create(GC::Ptr<Key> lower_bound, GC::Ptr<Key> upper_bound, LowerOpen lower_open, UpperOpen upper_open);

    bool lower_open() const { return m_lower_open; }
    bool upper_open() const { return m_upper_open; }

    bool is_unbound() const { return m_lower_bound == nullptr && m_upper_bound == nullptr; }
    bool is_in_range(GC::Ref<Key>) const;
    GC::Ptr<Key> lower_key() const { return m_lower_bound; }
    GC::Ptr<Key> upper_key() const { return m_upper_bound; }

protected:
    explicit IDBKeyRange(GC::Ptr<Key> lower_bound, GC::Ptr<Key> upper_bound, LowerOpen lower_open, UpperOpen upper_open);
    virtual void visit_edges(GC::Cell::Visitor& visitor) override;

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
