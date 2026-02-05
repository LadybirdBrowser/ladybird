/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SpeechRecognitionPrototype.h>
#include <LibWeb/Speech/SpeechRecognition.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Speech {

GC_DEFINE_ALLOCATOR(SpeechRecognition);

WebIDL::ExceptionOr<GC::Ref<SpeechRecognition>> SpeechRecognition::construct_impl(JS::Realm& realm)
{
    return realm.create<SpeechRecognition>(realm);
}

SpeechRecognition::SpeechRecognition(JS::Realm& realm)
    : DOM::EventTarget(realm)
{
}

SpeechRecognition::~SpeechRecognition() = default;

void SpeechRecognition::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SpeechRecognition);
    Base::initialize(realm);

    m_grammars = realm.create<SpeechGrammarList>(realm);
}

void SpeechRecognition::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_grammars);
}

#undef __ENUMERATE
#define __ENUMERATE(attribute_name, event_name)                                       \
    void SpeechRecognition::set_##attribute_name(GC::Ptr<WebIDL::CallbackType> value) \
    {                                                                                 \
        set_event_handler_attribute(event_name, value);                               \
    }                                                                                 \
    GC::Ptr<WebIDL::CallbackType> SpeechRecognition::attribute_name()                 \
    {                                                                                 \
        return event_handler_attribute(event_name);                                   \
    }
ENUMERATE_SPEECH_RECOGNITION_EVENT_HANDLERS(__ENUMERATE)
#undef __ENUMERATE

}
