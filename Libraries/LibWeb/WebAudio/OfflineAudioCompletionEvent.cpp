/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/OfflineAudioCompletionEvent.h>
#include <LibWeb/WebAudio/OfflineAudioCompletionEvent.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(OfflineAudioCompletionEvent);

WebIDL::ExceptionOr<GC::Ref<OfflineAudioCompletionEvent>> OfflineAudioCompletionEvent::construct_impl(JS::Realm& realm, FlyString const& event_name, Bindings::OfflineAudioCompletionEventInit const& event_init)
{
    return realm.create<OfflineAudioCompletionEvent>(realm, event_name, event_init);
}

OfflineAudioCompletionEvent::OfflineAudioCompletionEvent(JS::Realm& realm, FlyString const& event_name, Bindings::OfflineAudioCompletionEventInit const& event_init)
    : DOM::Event(realm, event_name, event_init)
    , m_rendered_buffer(event_init.rendered_buffer)
{
}

OfflineAudioCompletionEvent::~OfflineAudioCompletionEvent() = default;

void OfflineAudioCompletionEvent::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_rendered_buffer);
}

}
