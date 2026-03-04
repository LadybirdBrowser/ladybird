/*
 * Copyright (c) 2024, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibGC/Root.h>
#include <LibWeb/IndexedDB/IDBRequest.h>

namespace Web::IndexedDB {

class RequestList final {
    AK_MAKE_NONMOVABLE(RequestList);
    AK_MAKE_NONCOPYABLE(RequestList);

public:
    RequestList() = default;

    void enqueue(GC::Ref<IDBRequest> request, GC::Ref<GC::Function<void()>> steps);
    void remove(GC::Ref<IDBRequest> request);

    void on_request_processed();

    bool is_empty() const;

    void block_execution() { m_blocked = true; }
    bool execution_is_blocked() const { return m_blocked; }
    void unblock_execution();

    void set_on_all_processed(GC::Ref<GC::Function<void()>> callback);
    void clear_on_all_processed();
    void check_all_processed();
    void maybe_process_next_request();

private:
    struct Entry {
        GC::Weak<IDBRequest> request;
        GC::Root<GC::Function<void()>> steps;
    };

    Vector<Entry> m_entries;
    GC::Root<GC::Function<void()>> m_on_all_processed;
    bool m_blocked { false };

public:
    class RequestIterator {
    public:
        RequestIterator(Vector<Entry> const& entries, size_t index)
            : m_entries(entries)
            , m_index(index)
        {
            skip_null();
        }

        GC::Ref<IDBRequest> operator*() const;
        RequestIterator& operator++();
        bool operator!=(RequestIterator const& other) const { return m_index != other.m_index; }

    private:
        void skip_null();

        Vector<Entry> const& m_entries;
        size_t m_index;
    };

    RequestIterator begin() const { return { m_entries, 0 }; }
    RequestIterator end() const { return { m_entries, m_entries.size() }; }
};

}
