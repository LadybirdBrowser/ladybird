/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibGC/Cell.h>
#include <LibGC/Function.h>
#include <LibGC/Ptr.h>
#include <LibGC/RootVector.h>
#include <LibJS/Runtime/ExternalMemory.h>
#include <LibURL/URL.h>

namespace Web {

template<typename Value>
class ResourceCache {
public:
    ResourceCache(size_t count_limit, size_t memory_limit, Function<size_t(Value const&)> memory_size_of_value)
        : m_count_limit(count_limit)
        , m_memory_limit(memory_limit)
        , m_memory_size_of_value(move(memory_size_of_value))
    {
    }

    Optional<Value> peek(URL::URL const& url) const
    {
        auto it = m_entries.find(url);
        if (it == m_entries.end())
            return {};
        return it->value.value;
    }

    Optional<Value> get(URL::URL const& url)
    {
        auto it = m_entries.find(url);
        if (it == m_entries.end())
            return {};
        it->value.last_use_serial = ++m_use_serial;
        ++m_hit_count;
        auto value = it->value.value;
        evict_entries_to_fit_limits();
        return value;
    }

    void set(URL::URL const& url, Value value)
    {
        auto pending_load_callbacks = take_pending_load_callbacks(url);
        auto url_memory_size = url.serialize().byte_count();
        if (JS::saturating_add_external_memory_size(url_memory_size, m_memory_size_of_value(value)) <= m_memory_limit)
            insert(url, value, url_memory_size);
        for (auto const& callback : pending_load_callbacks)
            callback->function()(value);
    }

    using PendingLoadCallback = GC::Function<void(Optional<Value>)>;

    bool has_pending_load(URL::URL const& url) const { return m_pending_loads.contains(url); }

    void begin_pending_load(URL::URL const& url)
    {
        m_pending_loads.ensure(url);
    }

    void wait_for_pending_load(URL::URL const& url, GC::Ref<PendingLoadCallback> callback)
    {
        auto it = m_pending_loads.find(url);
        VERIFY(it != m_pending_loads.end());
        it->value.append(callback);
        ++m_hit_count;
    }

    void finish_pending_load_without_value(URL::URL const& url)
    {
        for (auto const& callback : take_pending_load_callbacks(url))
            callback->function()({});
    }

    void visit_edges(GC::Cell::Visitor& visitor)
    {
        if constexpr (requires(Value const& value) { visitor.visit(value); }) {
            for (auto const& it : m_entries)
                visitor.visit(it.value.value);
        }
        for (auto const& it : m_pending_loads)
            visitor.visit(it.value);
    }

    bool contains(URL::URL const& url) const { return m_entries.contains(url); }

    Optional<size_t> entry_memory_size(URL::URL const& url) const
    {
        auto it = m_entries.find(url);
        if (it == m_entries.end())
            return {};
        return memory_size_of(it->value);
    }

    u64 hit_count() const { return m_hit_count; }
    size_t memory_limit() const { return m_memory_limit; }

    void evict_entries_to_fit_limits()
    {
        size_t memory_size = 0;
        m_entries.remove_all_matching([&](URL::URL const&, Entry& entry) {
            entry.memory_size = memory_size_of(entry);
            if (entry.memory_size > m_memory_limit)
                return true;
            memory_size += entry.memory_size;
            return false;
        });

        while (m_entries.size() > m_count_limit || memory_size > m_memory_limit) {
            auto least_recently_used = m_entries.begin();
            for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
                if (it->value.last_use_serial < least_recently_used->value.last_use_serial)
                    least_recently_used = it;
            }
            memory_size -= least_recently_used->value.memory_size;
            m_entries.remove(least_recently_used);
        }
    }

private:
    struct Entry {
        Value value;
        size_t url_memory_size { 0 };
        size_t memory_size { 0 };
        u64 last_use_serial { 0 };
    };

    size_t memory_size_of(Entry const& entry) const
    {
        return JS::saturating_add_external_memory_size(entry.url_memory_size, m_memory_size_of_value(entry.value));
    }

    void insert(URL::URL const& url, Value value, size_t url_memory_size)
    {
        m_entries.set(url, { .value = move(value), .url_memory_size = url_memory_size, .last_use_serial = ++m_use_serial });
        evict_entries_to_fit_limits();
    }

    GC::RootVector<GC::Ref<PendingLoadCallback>> take_pending_load_callbacks(URL::URL const& url)
    {
        auto callbacks = m_pending_loads.take(url);
        if (!callbacks.has_value())
            return {};
        return GC::RootVector<GC::Ref<PendingLoadCallback>> { callbacks->span() };
    }

    size_t m_count_limit { 0 };
    size_t m_memory_limit { 0 };
    Function<size_t(Value const&)> m_memory_size_of_value;

    HashMap<URL::URL, Entry> m_entries;
    HashMap<URL::URL, Vector<GC::Ref<PendingLoadCallback>>> m_pending_loads;
    u64 m_use_serial { 0 };
    u64 m_hit_count { 0 };
};

}
