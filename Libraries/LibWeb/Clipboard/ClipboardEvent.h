/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Heap/GCPtr.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/DataTransfer.h>

namespace Web::Clipboard {

struct ClipboardEventInit : public DOM::EventInit {
    JS::GCPtr<HTML::DataTransfer> clipboard_data;
};

// https://w3c.github.io/clipboard-apis/#clipboardevent
class ClipboardEvent : public DOM::Event {
    WEB_PLATFORM_OBJECT(ClipboardEvent, DOM::Event);
    JS_DECLARE_ALLOCATOR(ClipboardEvent);

public:
    static JS::NonnullGCPtr<ClipboardEvent> construct_impl(JS::Realm&, FlyString const& event_name, ClipboardEventInit const& event_init);

    virtual ~ClipboardEvent() override;

    JS::GCPtr<HTML::DataTransfer> clipboard_data() { return m_clipboard_data; }

private:
    ClipboardEvent(JS::Realm&, FlyString const& event_name, ClipboardEventInit const& event_init);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(JS::Cell::Visitor&) override;

    JS::GCPtr<HTML::DataTransfer> m_clipboard_data;
};

}
