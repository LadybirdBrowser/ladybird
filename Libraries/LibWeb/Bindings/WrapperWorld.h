/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibGC/Cell.h>
#include <LibGC/CellAllocator.h>
#include <LibGC/Ptr.h>
#include <LibGC/Root.h>
#include <LibGC/Weak.h>
#include <LibGC/WeakInlines.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::Bindings {

class WEB_API WrapperWorld final : public GC::Cell {
    GC_CELL(WrapperWorld, GC::Cell);
    GC_DECLARE_ALLOCATOR(WrapperWorld);

public:
    using Type = WrapperWorldType;

    explicit WrapperWorld(Type);
    virtual ~WrapperWorld() override;

    [[nodiscard]] Type type() const { return m_type; }
    [[nodiscard]] bool is_main_world() const { return m_type == Type::Main; }

    [[nodiscard]] GC::Ptr<PlatformObject> wrapper_for(Wrappable const&, JS::Realm&) const;
    void set_wrapper(Wrappable&, PlatformObject&);
    void clear_wrapper(Wrappable&, PlatformObject const&);

private:
    virtual void visit_edges(GC::Cell::Visitor&) override;

    struct WrapperEntry {
        GC::Weak<Wrappable const> wrappable;
        GC::Weak<PlatformObject> wrapper;
    };

    Type m_type { Type::Main };
    Vector<WrapperEntry> m_wrappers;
};

WEB_API WrapperWorld& host_defined_wrapper_world(JS::Realm&);
WEB_API WrapperWorld const& host_defined_wrapper_world(JS::Realm const&);

template<typename T>
class WrapperWorldWeakValueCache {
public:
    [[nodiscard]] GC::Ptr<T> get(WrapperWorld const& wrapper_world)
    {
        if (wrapper_world.is_main_world())
            return m_main_world_value.ptr();

        prune();
        for (auto const& entry : m_values) {
            if (entry.wrapper_world.ptr() == &wrapper_world)
                return entry.value.ptr();
        }

        return nullptr;
    }

    void set(WrapperWorld const& wrapper_world, GC::Ptr<T> value)
    {
        if (wrapper_world.is_main_world()) {
            m_main_world_value = value.ptr();
            return;
        }

        prune();
        m_values.remove_all_matching([&](auto const& entry) {
            return entry.wrapper_world.ptr() == &wrapper_world;
        });

        if (value)
            m_values.append(Entry { wrapper_world, value.ptr() });
    }

    template<typename Callback>
    void for_each(Callback callback)
    {
        if (m_main_world_value) {
            auto value = GC::make_root(*m_main_world_value);
            callback(*value);
        }

        prune();
        for (auto const& entry : m_values) {
            auto value = GC::make_root(*entry.value);
            callback(*value);
        }
    }

    void clear()
    {
        m_main_world_value = nullptr;
        m_values.clear();
    }

private:
    struct Entry {
        GC::Weak<WrapperWorld> wrapper_world;
        GC::Weak<T> value;
    };

    void prune()
    {
        m_values.remove_all_matching([](auto const& entry) {
            return !entry.wrapper_world || !entry.value;
        });
    }

    GC::Weak<T> m_main_world_value;
    Vector<Entry> m_values;
};

template<typename Key, typename Value>
class WrapperWorldWeakValueCacheMap {
public:
    [[nodiscard]] WrapperWorldWeakValueCache<Value>& cache_for(Key& key)
    {
        prune();
        for (auto& entry : m_entries) {
            if (entry.key.ptr() == &key)
                return entry.cache;
        }

        m_entries.append(Entry { key, {} });
        return m_entries.last().cache;
    }

private:
    struct Entry {
        GC::Weak<Key> key;
        WrapperWorldWeakValueCache<Value> cache;
    };

    void prune()
    {
        m_entries.remove_all_matching([](auto const& entry) {
            return !entry.key;
        });
    }

    Vector<Entry> m_entries;
};

}
