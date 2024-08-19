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
    GC::Ptr<HTMLElement> submitter;
};

class SubmitEvent final : public DOM::Event {
    WEB_PLATFORM_OBJECT(SubmitEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(SubmitEvent);

public:
    [[nodiscard]] static GC::Ref<SubmitEvent> create(JS::Realm&, FlyString const& event_name, SubmitEventInit const& event_init);
    static WebIDL::ExceptionOr<GC::Ref<SubmitEvent>> construct_impl(JS::Realm&, FlyString const& event_name, SubmitEventInit const& event_init);

    virtual ~SubmitEvent() override;

    GC::Ptr<HTMLElement> submitter() const { return m_submitter; }

private:
    SubmitEvent(JS::Realm&, FlyString const& event_name, SubmitEventInit const& event_init);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ptr<HTMLElement> m_submitter;
};

}
