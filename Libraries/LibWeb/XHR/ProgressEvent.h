/*
 * Copyright (c) 2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <LibJS/Forward.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::HTML {

class WindowOrWorkerGlobalScopeMixin;

}

namespace Web::XHR {

class ProgressEvent final : public DOM::Event {
    WEB_WRAPPABLE(ProgressEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(ProgressEvent);

public:
    [[nodiscard]] static GC::Ref<ProgressEvent> create(FlyString const& event_name, Bindings::ProgressEventInit const&,
        HighResolutionTime::DOMHighResTimeStamp);
    static WebIDL::ExceptionOr<GC::Ref<ProgressEvent>> construct_impl(HTML::WindowOrWorkerGlobalScopeMixin&, FlyString const& event_name, Bindings::ProgressEventInit const&);

    virtual ~ProgressEvent() override;

    bool length_computable() const { return m_length_computable; }
    WebIDL::Double loaded() const { return m_loaded; }
    WebIDL::Double total() const { return m_total; }

private:
    ProgressEvent(FlyString const& event_name, Bindings::ProgressEventInit const&, HighResolutionTime::DOMHighResTimeStamp);

    bool m_length_computable { false };
    WebIDL::Double m_loaded { 0 };
    WebIDL::Double m_total { 0 };
};

}
