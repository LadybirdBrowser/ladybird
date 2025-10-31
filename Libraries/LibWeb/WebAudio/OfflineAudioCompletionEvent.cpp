/*
 * Copyright (c) 2025, Ben Eidson <b.e.eidson@gmail.com>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/OfflineAudioCompletionEventPrototype.h>
#include <LibWeb/WebAudio/OfflineAudioCompletionEvent.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(OfflineAudioCompletionEvent);

GC::Ref<OfflineAudioCompletionEvent> OfflineAudioCompletionEvent::create(JS::Realm& realm, FlyString const& type, OfflineAudioCompletionEventInit const& event_init)
{
    return realm.create<OfflineAudioCompletionEvent>(realm, type, event_init);
}

WebIDL::ExceptionOr<GC::Ref<OfflineAudioCompletionEvent>> OfflineAudioCompletionEvent::construct_impl(JS::Realm& realm, FlyString const& type, OfflineAudioCompletionEventInit const& event_init)
{
    return create(realm, type, event_init);
}

OfflineAudioCompletionEvent::OfflineAudioCompletionEvent(JS::Realm& realm, FlyString const& type, OfflineAudioCompletionEventInit const& event_init)
    : DOM::Event(realm, type, event_init)
    , m_rendered_buffer(*event_init.rendered_buffer)
{
}

OfflineAudioCompletionEvent::~OfflineAudioCompletionEvent() = default;

void OfflineAudioCompletionEvent::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(OfflineAudioCompletionEvent);
    Base::initialize(realm);
}

void OfflineAudioCompletionEvent::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_rendered_buffer);
}

}
