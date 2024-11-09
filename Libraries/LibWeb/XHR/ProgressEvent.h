/*
 * Copyright (c) 2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::XHR {

struct ProgressEventInit : public DOM::EventInit {
    bool length_computable { false };
    WebIDL::UnsignedLongLong loaded { 0 };
    WebIDL::UnsignedLongLong total { 0 };
};

class ProgressEvent final : public DOM::Event {
    WEB_PLATFORM_OBJECT(ProgressEvent, DOM::Event);
    JS_DECLARE_ALLOCATOR(ProgressEvent);

public:
    [[nodiscard]] static JS::NonnullGCPtr<ProgressEvent> create(JS::Realm&, FlyString const& event_name, ProgressEventInit const& event_init);
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<ProgressEvent>> construct_impl(JS::Realm&, FlyString const& event_name, ProgressEventInit const& event_init);

    virtual ~ProgressEvent() override;

    bool length_computable() const { return m_length_computable; }
    WebIDL::UnsignedLongLong loaded() const { return m_loaded; }
    WebIDL::UnsignedLongLong total() const { return m_total; }

private:
    ProgressEvent(JS::Realm&, FlyString const& event_name, ProgressEventInit const& event_init);

    virtual void initialize(JS::Realm&) override;

    bool m_length_computable { false };
    WebIDL::UnsignedLongLong m_loaded { 0 };
    WebIDL::UnsignedLongLong m_total { 0 };
};

}
