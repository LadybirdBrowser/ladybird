/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SpeechRecognitionEventPrototype.h>
#include <LibWeb/Speech/SpeechRecognitionEvent.h>

namespace Web::Speech {

GC_DEFINE_ALLOCATOR(SpeechRecognitionEvent);

GC::Ref<SpeechRecognitionEvent> SpeechRecognitionEvent::create(JS::Realm& realm, FlyString const& event_name, SpeechRecognitionEventInit event_init)
{
    return realm.create<SpeechRecognitionEvent>(realm, event_name, move(event_init));
}

WebIDL::ExceptionOr<GC::Ref<SpeechRecognitionEvent>> SpeechRecognitionEvent::construct_impl(JS::Realm& realm, FlyString const& event_name, SpeechRecognitionEventInit event_init)
{
    return create(realm, event_name, move(event_init));
}

SpeechRecognitionEvent::SpeechRecognitionEvent(JS::Realm& realm, FlyString const& event_name, SpeechRecognitionEventInit event_init)
    : DOM::Event(realm, event_name, event_init)
    , m_result_index(event_init.result_index)
    , m_results(event_init.results)
{
}

void SpeechRecognitionEvent::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SpeechRecognitionEvent);
    Base::initialize(realm);
}

void SpeechRecognitionEvent::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_results);
}

}
