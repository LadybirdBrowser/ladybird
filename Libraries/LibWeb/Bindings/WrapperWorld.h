/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/OwnPtr.h>
#include <LibGC/Cell.h>
#include <LibGC/CellAllocator.h>
#include <LibGC/Ptr.h>
#include <LibGC/Root.h>
#include <LibGC/WeakHashMap.h>
#include <LibGC/WeakHashSet.h>
#include <LibGC/WeakInlines.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::Bindings {

class WEB_API WrapperWorld final : public GC::Cell {
    GC_CELL(WrapperWorld, GC::Cell);
    GC_DECLARE_ALLOCATOR(WrapperWorld);

public:
    static constexpr bool OVERRIDES_FINALIZE = true;

    using Type = WrapperWorldType;

    explicit WrapperWorld(Type);
    // WrapperWorld destruction intentionally does not detach the world. Non-main
    // worlds that may hold preserved wrappers must be explicitly detached before
    // the embedder drops the world; the destructor verifies this except during
    // CollectEverything collection, where it is not safe to walk weak
    // wrappable references and mutate their preserved-wrapper lists.
    virtual ~WrapperWorld() override;

    [[nodiscard]] Type type() const { return m_type; }
    [[nodiscard]] bool is_main_world() const { return m_type == Type::Main; }
    [[nodiscard]] bool is_detached() const { return m_detached; }
    // Clear all preservation edges and cache entries owned by this non-main
    // world. Main worlds are agent-lifetime objects and must not detach.
    void detach();

    [[nodiscard]] GC::Ptr<PlatformObject> wrapper_for(Wrappable const&, JS::Realm&) const;
    void set_wrapper(Wrappable&, PlatformObject&);
    void clear_wrapper(Wrappable&, PlatformObject const&);
    void register_preserved_wrappable(GCAllocatedWrappable&);

private:
    virtual void finalize() override;

    Type m_type { Type::Main };
    bool m_detached { false };
    // Main-world cache cells are agent-local and may span same-agent realms.
    // Non-main cache cells are realm-local; a logical isolated world that spans
    // multiple realms must allocate one WrapperWorld cell per realm. Both sides
    // are weak; the cache cannot keep either implementation or wrapper alive.
    GC::WeakHashMap<Wrappable, PlatformObject> m_wrappers;
    GC::WeakHashSet<GCAllocatedWrappable> m_preserved_wrappables;
};

WEB_API WrapperWorld& host_defined_wrapper_world(JS::Realm&);
WEB_API WrapperWorld const& host_defined_wrapper_world(JS::Realm const&);

template<typename T>
class WrapperWorldWeakValueCache {
public:
    [[nodiscard]] GC::Ptr<T> get(WrapperWorld const& wrapper_world)
    {
        return m_values.get(wrapper_world);
    }

    void set(WrapperWorld const& wrapper_world, GC::Ptr<T> value)
    {
        if (value)
            m_values.set(wrapper_world, *value);
        else
            m_values.remove(wrapper_world);
    }

    template<typename Callback>
    void for_each(Callback callback)
    {
        m_values.for_each_live_value([&](T& live_value) {
            auto value = GC::make_root(live_value);
            callback(*value);
        });
    }

    void clear()
    {
        m_values.clear();
    }

private:
    GC::WeakHashMap<WrapperWorld, T> m_values;
};

template<typename Key, typename Value>
class WrapperWorldWeakValueCacheMap {
public:
    // Returned cache references stay valid across rehashes (the caches live behind
    // an OwnPtr), but callers must not hold one across later calls into this map:
    // cache_for() and for_each() prune dead keys and may remove the referenced entry.
    [[nodiscard]] WrapperWorldWeakValueCache<Value>& cache_for(Key& key)
    {
        return *m_entries.ensure(key, [] { return make<WrapperWorldWeakValueCache<Value>>(); });
    }

    template<typename Callback>
    void for_each(Callback callback)
    {
        // The callback must not reenter this map through cache_for() or
        // for_each(); pruning or insertion would invalidate this iteration.
        m_entries.for_each_live_value([&](auto& cache) { callback(*cache); });
    }

private:
    // The per-key cache is held behind an OwnPtr so this map can be embedded as a
    // member with only a forward declaration of Value (e.g. WindowProxy in
    // BrowsingContext). Value only needs to be complete where cache_for() is
    // instantiated and where this map is destroyed.
    GC::WeakHashMap<Key, NonnullOwnPtr<WrapperWorldWeakValueCache<Value>>> m_entries;
};

}
