/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Bindings/SubmitEvent.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/HighResolutionTime/DOMHighResTimeStamp.h>

namespace Web::HTML {

class Window;

class SubmitEvent final : public DOM::Event {
    WEB_WRAPPABLE(SubmitEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(SubmitEvent);

public:
    [[nodiscard]] static GC::Ref<SubmitEvent> create(FlyString const& event_name, Bindings::SubmitEventInit const&, HighResolutionTime::DOMHighResTimeStamp);
    static WebIDL::ExceptionOr<GC::Ref<SubmitEvent>> construct_impl(Window&, FlyString const& event_name, Bindings::SubmitEventInit const&);

    virtual ~SubmitEvent() override;

    GC::Ptr<HTMLElement> submitter() const { return m_submitter; }

private:
    SubmitEvent(FlyString const& event_name, Bindings::SubmitEventInit const&, HighResolutionTime::DOMHighResTimeStamp);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    GC::Ptr<HTMLElement> m_submitter;
};

}
