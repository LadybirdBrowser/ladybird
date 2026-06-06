/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HighResolutionTime/DOMHighResTimeStamp.h>

namespace Web::HTML {

class WindowOrWorkerGlobalScopeMixin;

}

namespace Web::CSS {

class FontFaceSetLoadEvent : public DOM::Event {
    WEB_WRAPPABLE(FontFaceSetLoadEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(FontFaceSetLoadEvent);

public:
    [[nodiscard]] static GC::Ref<FontFaceSetLoadEvent> create(FlyString const& type, Bindings::FontFaceSetLoadEventInit const&, HighResolutionTime::DOMHighResTimeStamp);
    static WebIDL::ExceptionOr<GC::Ref<FontFaceSetLoadEvent>> construct_impl(HTML::WindowOrWorkerGlobalScopeMixin&, FlyString const& type, Bindings::FontFaceSetLoadEventInit const&);

    virtual ~FontFaceSetLoadEvent() override = default;

    Vector<GC::Ref<FontFace>> const& fontfaces() const { return m_fontfaces; }

private:
    FontFaceSetLoadEvent(FlyString const& type, Bindings::FontFaceSetLoadEventInit const&, HighResolutionTime::DOMHighResTimeStamp);

    virtual void visit_edges(Visitor&) override;

    Vector<GC::Ref<FontFace>> m_fontfaces;
};

}
