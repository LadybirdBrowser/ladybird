/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Bindings/SpeechRecognitionEvent.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/Speech/SpeechRecognitionResultList.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::Speech {

class SpeechRecognitionEvent : public DOM::Event {
    WEB_PLATFORM_OBJECT(SpeechRecognitionEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(SpeechRecognitionEvent);

public:
    [[nodiscard]] static GC::Ref<SpeechRecognitionEvent> create(JS::Realm&, FlyString const& event_name, Bindings::SpeechRecognitionEventInit const& = {});
    static WebIDL::ExceptionOr<GC::Ref<SpeechRecognitionEvent>> construct_impl(JS::Realm&, FlyString const& event_name, Bindings::SpeechRecognitionEventInit const&);

    // https://wicg.github.io/speech-api/#dom-speechrecognitionevent-resultindex
    WebIDL::UnsignedLong result_index() const { return m_result_index; }

    // https://wicg.github.io/speech-api/#dom-speechrecognitionevent-results
    GC::Ptr<SpeechRecognitionResultList> results() const { return m_results; }

private:
    SpeechRecognitionEvent(JS::Realm&, FlyString const& event_name, Bindings::SpeechRecognitionEventInit const&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    WebIDL::UnsignedLong m_result_index { 0 };
    GC::Ptr<SpeechRecognitionResultList> m_results;
};

}
