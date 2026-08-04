/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Speech/SpeechRecognition.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Speech {

GC_DEFINE_ALLOCATOR(SpeechRecognition);

GC::Ref<SpeechRecognition> SpeechRecognition::create()
{
    return GC::Heap::the().allocate<SpeechRecognition>();
}

SpeechRecognition::SpeechRecognition()
    : DOM::EventTarget()
    , m_grammars(SpeechGrammarList::create())
{
}

SpeechRecognition::~SpeechRecognition() = default;

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
