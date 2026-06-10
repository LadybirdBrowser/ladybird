/*
 * Copyright (c) 2023, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Bindings/FormDataEvent.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HighResolutionTime/DOMHighResTimeStamp.h>
#include <LibWeb/XHR/FormData.h>

namespace Web::HTML {

using FormDataEventInit = Bindings::FormDataEventInit;

class FormDataEvent final : public DOM::Event {
    WEB_WRAPPABLE(FormDataEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(FormDataEvent);

public:
    static GC::Ref<FormDataEvent> create(FlyString const& event_name, FormDataEventInit const&, HighResolutionTime::DOMHighResTimeStamp);

    virtual ~FormDataEvent() override;

    GC::Ptr<XHR::FormData> form_data() const { return m_form_data; }

private:
    FormDataEvent(FlyString const& event_name, FormDataEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    GC::Ptr<XHR::FormData> m_form_data;
};

}
