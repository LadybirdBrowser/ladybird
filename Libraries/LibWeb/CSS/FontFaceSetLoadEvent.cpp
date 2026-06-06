/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/FontFaceSetLoadEvent.h>
#include <LibWeb/CSS/FontFace.h>
#include <LibWeb/CSS/FontFaceSetLoadEvent.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(FontFaceSetLoadEvent);

GC::Ref<FontFaceSetLoadEvent> FontFaceSetLoadEvent::create(FlyString const& event_name, Bindings::FontFaceSetLoadEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<FontFaceSetLoadEvent>(event_name, event_init, time_stamp);
}

// https://drafts.csswg.org/css-font-loading/#dom-fontfacesetloadevent-fontfacesetloadevent
WebIDL::ExceptionOr<GC::Ref<FontFaceSetLoadEvent>> FontFaceSetLoadEvent::construct_impl(HTML::WindowOrWorkerGlobalScopeMixin& global_scope, FlyString const& event_name, Bindings::FontFaceSetLoadEventInit const& event_init)
{
    return create(event_name, event_init, HighResolutionTime::current_high_resolution_time(HTML::relevant_global_object(global_scope)));
}

FontFaceSetLoadEvent::FontFaceSetLoadEvent(FlyString const& event_name, Bindings::FontFaceSetLoadEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(event_name, event_init, time_stamp)
{
    m_fontfaces.ensure_capacity(event_init.fontfaces.size());
    for (auto const& font_face : event_init.fontfaces) {
        m_fontfaces.unchecked_append(font_face);
    }
}

void FontFaceSetLoadEvent::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_fontfaces);
}

}
