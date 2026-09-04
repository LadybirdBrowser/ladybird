/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/SpeechRecognitionEvent.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HighResolutionTime/DOMHighResTimeStamp.h>
#include <LibWeb/Speech/SpeechRecognitionResultList.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::HTML {

class Window;

}

namespace Web::Speech {

using SpeechRecognitionEventInit = Bindings::SpeechRecognitionEventInit;

class SpeechRecognitionEvent : public DOM::Event {
    WEB_WRAPPABLE(SpeechRecognitionEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(SpeechRecognitionEvent);

public:
    static constexpr size_t results_offset() { return offsetof(SpeechRecognitionEvent, m_results); }
    static GC::Ref<SpeechRecognitionEvent> create(FlyString const& event_name, SpeechRecognitionEventInit const&, HighResolutionTime::DOMHighResTimeStamp);
    static GC::Ref<SpeechRecognitionEvent> create(Utf16String const& event_name, SpeechRecognitionEventInit const&, HighResolutionTime::DOMHighResTimeStamp);
    static GC::Ref<SpeechRecognitionEvent> create(Utf16FlyString const& event_name, SpeechRecognitionEventInit const&);

    // https://wicg.github.io/speech-api/#dom-speechrecognitionevent-resultindex
    WebIDL::UnsignedLong result_index() const { return m_result_index; }

    // https://wicg.github.io/speech-api/#dom-speechrecognitionevent-results
    GC::Ref<SpeechRecognitionResultList> results() const { return m_results; }

private:
    SpeechRecognitionEvent(FlyString const& event_name, SpeechRecognitionEventInit const&, HighResolutionTime::DOMHighResTimeStamp);
    SpeechRecognitionEvent(Utf16FlyString const& event_name, SpeechRecognitionEventInit const&, HighResolutionTime::DOMHighResTimeStamp);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    WebIDL::UnsignedLong m_result_index { 0 };
    GC::Ref<SpeechRecognitionResultList> m_results;
};

}
