/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SpeechSynthesisPrototype.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/Speech/SpeechSynthesis.h>
#include <LibWeb/Speech/SpeechSynthesisVoice.h>

namespace Web::Speech {

GC_DEFINE_ALLOCATOR(SpeechSynthesis);

GC::Ref<SpeechSynthesis> SpeechSynthesis::create(JS::Realm& realm)
{
    return realm.create<SpeechSynthesis>(realm);
}

SpeechSynthesis::SpeechSynthesis(JS::Realm& realm)
    : DOM::EventTarget(realm)
{
}

SpeechSynthesis::~SpeechSynthesis() = default;

void SpeechSynthesis::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SpeechSynthesis);
    Base::initialize(realm);
}

void SpeechSynthesis::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_voices);
}

void SpeechSynthesis::set_onvoiceschanged(GC::Ptr<WebIDL::CallbackType> event_handler)
{
    set_event_handler_attribute(HTML::EventNames::voiceschanged, event_handler);
}

GC::Ptr<WebIDL::CallbackType> SpeechSynthesis::onvoiceschanged()
{
    return event_handler_attribute(HTML::EventNames::voiceschanged);
}

// https://wicg.github.io/speech-api/#dom-speechsynthesis-cancel
void SpeechSynthesis::cancel()
{
    dbgln("FIXME: Implement SpeechSynthesis::cancel()");
}

// https://wicg.github.io/speech-api/#dom-speechsynthesis-getvoices
Vector<GC::Ref<SpeechSynthesisVoice>> const& SpeechSynthesis::get_voices() const
{
    // FIXME: Populate m_voices with available synthesis voices.
    return m_voices;
}

}
