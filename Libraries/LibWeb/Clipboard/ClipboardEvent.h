/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/DataTransfer.h>

namespace Web::Clipboard {

struct ClipboardEventInit : public DOM::EventInit {
    GC::Ptr<HTML::DataTransfer> clipboard_data;
};

// https://w3c.github.io/clipboard-apis/#clipboardevent
class ClipboardEvent : public DOM::Event {
    WEB_PLATFORM_OBJECT(ClipboardEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(ClipboardEvent);

public:
    static GC::Ref<ClipboardEvent> construct_impl(JS::Realm&, FlyString const& event_name, ClipboardEventInit const& event_init);

    virtual ~ClipboardEvent() override;

    GC::Ptr<HTML::DataTransfer> clipboard_data() { return m_clipboard_data; }

private:
    ClipboardEvent(JS::Realm&, FlyString const& event_name, ClipboardEventInit const& event_init);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(JS::Cell::Visitor&) override;

    GC::Ptr<HTML::DataTransfer> m_clipboard_data;
};

}
