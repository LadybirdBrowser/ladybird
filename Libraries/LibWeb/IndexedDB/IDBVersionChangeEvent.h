/*
 * Copyright (c) 2024, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Heap/GCPtr.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/Forward.h>

namespace Web::IndexedDB {

struct IDBVersionChangeEventInit : public DOM::EventInit {
    u64 old_version { 0 };
    Optional<u64> new_version;
};

// https://w3c.github.io/IndexedDB/#events
class IDBVersionChangeEvent : public DOM::Event {
    WEB_PLATFORM_OBJECT(IDBVersionChangeEvent, DOM::Event);
    JS_DECLARE_ALLOCATOR(IDBVersionChangeEvent);

public:
    virtual ~IDBVersionChangeEvent() override;

    static JS::NonnullGCPtr<IDBVersionChangeEvent> create(JS::Realm&, FlyString const&, IDBVersionChangeEventInit const&);

    u64 old_version() const { return m_old_version; }
    Optional<u64> new_version() const { return m_new_version; }

protected:
    explicit IDBVersionChangeEvent(JS::Realm&, FlyString const& event_name, IDBVersionChangeEventInit const& event_init);

    virtual void initialize(JS::Realm&) override;

private:
    u64 m_old_version { 0 };
    Optional<u64> m_new_version;
};

}
