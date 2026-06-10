/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Speech/SpeechSynthesisUtterance.h>
#include <LibWeb/Speech/SpeechSynthesisVoice.h>

namespace Web::Speech {

GC_DEFINE_ALLOCATOR(SpeechSynthesisUtterance);

GC::Ref<SpeechSynthesisUtterance> SpeechSynthesisUtterance::create(String const& text)
{
    return GC::Heap::the().allocate<SpeechSynthesisUtterance>(text);
}

SpeechSynthesisUtterance::SpeechSynthesisUtterance(String const& text)
    : DOM::EventTarget()
    , m_text(text)
{
}

SpeechSynthesisUtterance::~SpeechSynthesisUtterance() = default;

void SpeechSynthesisUtterance::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_voice);
}

#undef __ENUMERATE
#define __ENUMERATE(attribute_name, event_name)                                              \
    void SpeechSynthesisUtterance::set_##attribute_name(GC::Ptr<WebIDL::CallbackType> value) \
    {                                                                                        \
        set_event_handler_attribute(event_name, value);                                      \
    }                                                                                        \
    GC::Ptr<WebIDL::CallbackType> SpeechSynthesisUtterance::attribute_name()                 \
    {                                                                                        \
        return event_handler_attribute(event_name);                                          \
    }
ENUMERATE_SPEECH_SYNTHESIS_UTTERANCE_EVENT_HANDLERS(__ENUMERATE)
#undef __ENUMERATE

}
