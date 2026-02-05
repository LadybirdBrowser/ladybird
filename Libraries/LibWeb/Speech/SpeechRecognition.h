/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/Speech/SpeechGrammarList.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebIDL/Types.h>

#define ENUMERATE_SPEECH_RECOGNITION_EVENT_HANDLERS(E) \
    E(onaudiostart, HTML::EventNames::audiostart)      \
    E(onsoundstart, HTML::EventNames::soundstart)      \
    E(onspeechstart, HTML::EventNames::speechstart)    \
    E(onspeechend, HTML::EventNames::speechend)        \
    E(onsoundend, HTML::EventNames::soundend)          \
    E(onaudioend, HTML::EventNames::audioend)          \
    E(onresult, HTML::EventNames::result)              \
    E(onnomatch, HTML::EventNames::nomatch)            \
    E(onerror, HTML::EventNames::error)                \
    E(onstart, HTML::EventNames::start)                \
    E(onend, HTML::EventNames::end)

namespace Web::Speech {

class SpeechRecognition final : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(SpeechRecognition, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(SpeechRecognition);

public:
    static WebIDL::ExceptionOr<GC::Ref<SpeechRecognition>> construct_impl(JS::Realm&);
    virtual ~SpeechRecognition() override;

    // https://wicg.github.io/speech-api/#dom-speechrecognition-grammars
    GC::Ref<SpeechGrammarList> grammars() const { return *m_grammars; }
    void set_grammars(GC::Ref<SpeechGrammarList> grammars) { m_grammars = grammars; }

    // https://wicg.github.io/speech-api/#dom-speechrecognition-lang
    String const& lang() const { return m_lang; }
    void set_lang(String const& lang) { m_lang = lang; }

    // https://wicg.github.io/speech-api/#dom-speechrecognition-continuous
    bool continuous() const { return m_continuous; }
    void set_continuous(bool continuous) { m_continuous = continuous; }

    // https://wicg.github.io/speech-api/#dom-speechrecognition-interimresults
    bool interim_results() const { return m_interim_results; }
    void set_interim_results(bool interim_results) { m_interim_results = interim_results; }

    // https://wicg.github.io/speech-api/#dom-speechrecognition-maxalternatives
    WebIDL::UnsignedLong max_alternatives() const { return m_max_alternatives; }
    void set_max_alternatives(WebIDL::UnsignedLong max_alternatives) { m_max_alternatives = max_alternatives; }

#undef __ENUMERATE
#define __ENUMERATE(attribute_name, event_name)               \
    void set_##attribute_name(GC::Ptr<WebIDL::CallbackType>); \
    GC::Ptr<WebIDL::CallbackType> attribute_name();
    ENUMERATE_SPEECH_RECOGNITION_EVENT_HANDLERS(__ENUMERATE)
#undef __ENUMERATE

private:
    explicit SpeechRecognition(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ptr<SpeechGrammarList> m_grammars;
    String m_lang;
    bool m_continuous { false };
    bool m_interim_results { false };
    WebIDL::UnsignedLong m_max_alternatives { 1 };
};

}
