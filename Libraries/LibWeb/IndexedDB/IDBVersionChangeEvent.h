/*
 * Copyright (c) 2024, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibGC/Ptr.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HighResolutionTime/DOMHighResTimeStamp.h>

namespace Web::Bindings {

struct IDBVersionChangeEventInit;

}

namespace Web::IndexedDB {

struct IDBVersionChangeEventInit : public DOM::EventInit {
    u64 old_version { 0 };
    Optional<u64> new_version;
};

// https://w3c.github.io/IndexedDB/#events
class IDBVersionChangeEvent : public DOM::Event {
    WEB_WRAPPABLE(IDBVersionChangeEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(IDBVersionChangeEvent);

public:
    virtual ~IDBVersionChangeEvent() override;

    static GC::Ref<IDBVersionChangeEvent> create(FlyString const&, IDBVersionChangeEventInit const&, HighResolutionTime::DOMHighResTimeStamp);
    static GC::Ref<IDBVersionChangeEvent> create(FlyString const&, Bindings::IDBVersionChangeEventInit const&, HighResolutionTime::DOMHighResTimeStamp);

    u64 old_version() const { return m_old_version; }
    Optional<u64> new_version() const { return m_new_version; }

protected:
    explicit IDBVersionChangeEvent(FlyString const& event_name, IDBVersionChangeEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp);

private:
    u64 m_old_version { 0 };
    Optional<u64> m_new_version;
};

}
