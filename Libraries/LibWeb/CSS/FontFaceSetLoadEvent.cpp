/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/FontFaceSetLoadEventPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/FontFace.h>
#include <LibWeb/CSS/FontFaceSetLoadEvent.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(FontFaceSetLoadEvent);

GC::Ref<FontFaceSetLoadEvent> FontFaceSetLoadEvent::create(JS::Realm& realm, FlyString const& event_name, FontFaceSetLoadEventInit const& event_init)
{
    return realm.create<FontFaceSetLoadEvent>(realm, event_name, event_init);
}

// https://drafts.csswg.org/css-font-loading/#dom-fontfacesetloadevent-fontfacesetloadevent
WebIDL::ExceptionOr<GC::Ref<FontFaceSetLoadEvent>> FontFaceSetLoadEvent::construct_impl(JS::Realm& realm, FlyString const& event_name, FontFaceSetLoadEventInit const& event_init)
{
    return create(realm, event_name, event_init);
}

FontFaceSetLoadEvent::FontFaceSetLoadEvent(JS::Realm& realm, FlyString const& event_name, CSS::FontFaceSetLoadEventInit const& event_init)
    : DOM::Event(realm, event_name, event_init)
{
    m_fontfaces.ensure_capacity(event_init.fontfaces.size());
    for (auto const& font_face : event_init.fontfaces) {
        VERIFY(font_face);
        m_fontfaces.unchecked_append(*font_face);
    }
}

void FontFaceSetLoadEvent::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(FontFaceSetLoadEvent);
    Base::initialize(realm);
}

void FontFaceSetLoadEvent::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_fontfaces);
}

}
