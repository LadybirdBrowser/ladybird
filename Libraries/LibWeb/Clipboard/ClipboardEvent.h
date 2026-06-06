/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/DataTransfer.h>
#include <LibWeb/HighResolutionTime/DOMHighResTimeStamp.h>

namespace Web::HTML {

class Window;

}

namespace Web::Clipboard {

// https://w3c.github.io/clipboard-apis/#clipboardevent
class ClipboardEvent : public DOM::Event {
    WEB_WRAPPABLE(ClipboardEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(ClipboardEvent);

public:
    static GC::Ref<ClipboardEvent> construct_impl(HTML::Window&, FlyString const& event_name, Bindings::ClipboardEventInit const& event_init);

    virtual ~ClipboardEvent() override;

    GC::Ptr<HTML::DataTransfer> clipboard_data() { return m_clipboard_data; }

private:
    ClipboardEvent(FlyString const& event_name, Bindings::ClipboardEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    GC::Ptr<HTML::DataTransfer> m_clipboard_data;
};

}
