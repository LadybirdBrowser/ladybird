/*
 * Copyright (c) 2025, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <LibGC/Heap.h>
#include <LibWeb/Bindings/ImplementedInBindings.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/CookieStore/CookieChangeEvent.h>

namespace Web::CookieStore {

GC_DEFINE_ALLOCATOR(CookieChangeEvent);

GC::Ref<CookieChangeEvent> CookieChangeEvent::create(FlyString const& event_name, CookieChangeEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<CookieChangeEvent>(event_name, event_init, time_stamp);
}

CookieChangeEvent::CookieChangeEvent(FlyString const& event_name, CookieChangeEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(event_name, event_init, time_stamp)
    , m_changed(event_init.changed.value_or({}))
    , m_deleted(event_init.deleted.value_or({}))
{
}

CookieChangeEvent::~CookieChangeEvent() = default;

}

namespace Web::Bindings {

struct CookieChangeEventCacheEntry {
    GC::Weak<CookieStore::CookieChangeEvent const> event;
    WrapperWorldWeakValueCache<JS::Object> changed_arrays;
    WrapperWorldWeakValueCache<JS::Object> deleted_arrays;
};

static Vector<CookieChangeEventCacheEntry>& cookie_change_event_caches()
{
    static NeverDestroyed<Vector<CookieChangeEventCacheEntry>> caches;
    return *caches;
}

static void prune_cookie_change_event_caches()
{
    cookie_change_event_caches().remove_all_matching([](auto const& entry) {
        return !entry.event;
    });
}

static CookieChangeEventCacheEntry& cookie_change_event_cache_for(CookieStore::CookieChangeEvent const& event)
{
    auto& caches = cookie_change_event_caches();
    prune_cookie_change_event_caches();

    for (auto& entry : caches) {
        if (entry.event.ptr() == &event)
            return entry;
    }

    caches.append(CookieChangeEventCacheEntry { event, {}, {} });
    return caches.last();
}

GC::Ptr<JS::Object> cached_changed(CookieStore::CookieChangeEvent const& event, WrapperWorld const& wrapper_world)
{
    return cookie_change_event_cache_for(event).changed_arrays.get(wrapper_world);
}

void set_cached_changed(CookieStore::CookieChangeEvent const& event, WrapperWorld const& wrapper_world, GC::Ptr<JS::Object> array)
{
    cookie_change_event_cache_for(event).changed_arrays.set(wrapper_world, array);
}

GC::Ptr<JS::Object> cached_deleted(CookieStore::CookieChangeEvent const& event, WrapperWorld const& wrapper_world)
{
    return cookie_change_event_cache_for(event).deleted_arrays.get(wrapper_world);
}

void set_cached_deleted(CookieStore::CookieChangeEvent const& event, WrapperWorld const& wrapper_world, GC::Ptr<JS::Object> array)
{
    cookie_change_event_cache_for(event).deleted_arrays.set(wrapper_world, array);
}

}
