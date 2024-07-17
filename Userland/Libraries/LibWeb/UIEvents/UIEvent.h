/*
 * Copyright (c) 2020-2022, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefPtr.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/Window.h>

namespace Web::UIEvents {

struct UIEventInit : public DOM::EventInit {
    JS::GCPtr<HTML::Window> view;
    int detail { 0 };
};

class UIEvent : public DOM::Event {
    WEB_PLATFORM_OBJECT(UIEvent, DOM::Event);
    JS_DECLARE_ALLOCATOR(UIEvent);

public:
    [[nodiscard]] static JS::NonnullGCPtr<UIEvent> create(JS::Realm&, FlyString const& type);
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<UIEvent>> construct_impl(JS::Realm&, FlyString const& event_name, UIEventInit const& event_init);

    virtual ~UIEvent() override;

    HTML::Window const* view() const { return m_view.ptr(); }
    int detail() const { return m_detail; }
    virtual u32 which() const { return 0; }

    void init_ui_event(String const& type, bool bubbles, bool cancelable, HTML::Window* view, int detail)
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

    JS::GCPtr<HTML::Window> m_view;
    int m_detail { 0 };
};

}
