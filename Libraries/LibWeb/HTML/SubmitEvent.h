/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/HTMLElement.h>

namespace Web::HTML {

struct SubmitEventInit : public DOM::EventInit {
    JS::GCPtr<HTMLElement> submitter;
};

class SubmitEvent final : public DOM::Event {
    WEB_PLATFORM_OBJECT(SubmitEvent, DOM::Event);
    JS_DECLARE_ALLOCATOR(SubmitEvent);

public:
    [[nodiscard]] static JS::NonnullGCPtr<SubmitEvent> create(JS::Realm&, FlyString const& event_name, SubmitEventInit const& event_init);
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<SubmitEvent>> construct_impl(JS::Realm&, FlyString const& event_name, SubmitEventInit const& event_init);

    virtual ~SubmitEvent() override;

    JS::GCPtr<HTMLElement> submitter() const { return m_submitter; }

private:
    SubmitEvent(JS::Realm&, FlyString const& event_name, SubmitEventInit const& event_init);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    JS::GCPtr<HTMLElement> m_submitter;
};

}
