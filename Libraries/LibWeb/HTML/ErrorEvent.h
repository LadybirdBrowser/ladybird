/*
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/String.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/Bindings/ErrorEvent.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HighResolutionTime/DOMHighResTimeStamp.h>

namespace Web::HTML {

using ErrorEventInit = Bindings::ErrorEventInit;

// https://html.spec.whatwg.org/multipage/webappapis.html#errorevent
class ErrorEvent final : public DOM::Event {
    WEB_WRAPPABLE(ErrorEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(ErrorEvent);

public:
    [[nodiscard]] static GC::Ref<ErrorEvent> create(FlyString const& event_name, ErrorEventInit const& = {}, HighResolutionTime::DOMHighResTimeStamp = 0);

    virtual ~ErrorEvent() override;

    // https://html.spec.whatwg.org/multipage/webappapis.html#dom-errorevent-message
    String const& message() const { return m_message; }

    // https://html.spec.whatwg.org/multipage/webappapis.html#dom-errorevent-filename
    String const& filename() const { return m_filename; }

    // https://html.spec.whatwg.org/multipage/webappapis.html#dom-errorevent-lineno
    u32 lineno() const { return m_lineno; }

    // https://html.spec.whatwg.org/multipage/webappapis.html#dom-errorevent-colno
    u32 colno() const { return m_colno; }

    // https://html.spec.whatwg.org/multipage/webappapis.html#dom-errorevent-error
    JS::Value const& error() const { return m_error; }

private:
    ErrorEvent(FlyString const& event_name, ErrorEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    String m_message;
    String m_filename;
    u32 m_lineno { 0 };
    u32 m_colno { 0 };
    JS::Value m_error;
};

}
