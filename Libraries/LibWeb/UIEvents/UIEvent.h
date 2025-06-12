/*
 * Copyright (c) 2020-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefPtr.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/Window.h>

namespace Web::UIEvents {

struct UIEventInit : public DOM::EventInit {
    GC::Ptr<HTML::WindowProxy> view;
    int detail { 0 };
};

class UIEvent : public DOM::Event {
    WEB_PLATFORM_OBJECT(UIEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(UIEvent);

public:
    [[nodiscard]] static GC::Ref<UIEvent> create(JS::Realm&, FlyString const& type);
    static WebIDL::ExceptionOr<GC::Ref<UIEvent>> construct_impl(JS::Realm&, FlyString const& event_name, UIEventInit const& event_init);

    virtual ~UIEvent() override;

    GC::Ptr<HTML::WindowProxy> const view() const { return m_view; }
    int detail() const { return m_detail; }
    virtual u32 which() const { return 0; }

    void init_ui_event(String const& type, bool bubbles, bool cancelable, GC::Ptr<HTML::WindowProxy> view, int detail)
    {
        // Initializes attributes of an UIEvent object. This method has the same behavior as initEvent().

        // 1. If this’s dispatch flag is set, then return.
        if (dispatched())
            return;

        // 2. Initialize this with type, bubbles, and cancelable.
        initialize_event(type, bubbles, cancelable);

        // Implementation Defined: Initialise other values.
        m_view = view;
        m_detail = detail;
    }

protected:
    UIEvent(JS::Realm&, FlyString const& event_name);
    UIEvent(JS::Realm&, FlyString const& event_name, UIEventInit const& event_init);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ptr<HTML::WindowProxy> m_view;
    int m_detail { 0 };
};

}
