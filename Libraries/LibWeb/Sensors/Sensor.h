/*
 * Copyright (c) 2025, Saksham Goyal <sakgoy2001@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Sensors {

class Sensor final : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(Sensor, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(Sensor);

public:
    static WebIDL::ExceptionOr<GC::Ref<Sensor>> construct_impl(JS::Realm&);
    virtual ~Sensor() override = default;

    auto const& activated() const { return m_activated; }
    auto const& has_reading() const { return m_has_reading; }
    auto const& timestamp() const { return m_timestamp; }

    void start() { }
    void stop() { }

    void set_onreading(WebIDL::CallbackType*);
    WebIDL::CallbackType* onreading();

    void set_onactivate(WebIDL::CallbackType*);
    WebIDL::CallbackType* onactivate();

    void set_onerror(WebIDL::CallbackType*);
    WebIDL::CallbackType* onerror();

private:
    Sensor(JS::Realm&);

    virtual void initialize(JS::Realm&) override;

    bool m_activated;
    bool m_has_reading;
    Optional<HighResolutionTime::DOMHighResTimeStamp> m_timestamp;
};

}
