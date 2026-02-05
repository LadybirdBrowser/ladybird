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
#include <LibWeb/WebIDL/ExceptionOr.h>

#define ENUMERATE_SPEECH_SYNTHESIS_UTTERANCE_EVENT_HANDLERS(E) \
    E(onstart, HTML::EventNames::start)                        \
    E(onend, HTML::EventNames::end)                            \
    E(onerror, HTML::EventNames::error)                        \
    E(onpause, HTML::EventNames::pause)                        \
    E(onresume, HTML::EventNames::resume)                      \
    E(onmark, HTML::EventNames::mark)                          \
    E(onboundary, HTML::EventNames::boundary)

namespace Web::Speech {

class SpeechSynthesisUtterance final : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(SpeechSynthesisUtterance, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(SpeechSynthesisUtterance);

public:
    static WebIDL::ExceptionOr<GC::Ref<SpeechSynthesisUtterance>> construct_impl(JS::Realm&, String const& text = {});
    virtual ~SpeechSynthesisUtterance() override;

    // https://wicg.github.io/speech-api/#dom-speechsynthesisutterance-text
    String const& text() const { return m_text; }
    void set_text(String const& text) { m_text = text; }

    // https://wicg.github.io/speech-api/#dom-speechsynthesisutterance-lang
    String const& lang() const { return m_lang; }
    void set_lang(String const& lang) { m_lang = lang; }

    // https://wicg.github.io/speech-api/#dom-speechsynthesisutterance-voice
    GC::Ptr<SpeechSynthesisVoice> voice() const { return m_voice; }
    void set_voice(GC::Ptr<SpeechSynthesisVoice> voice) { m_voice = voice; }

    // https://wicg.github.io/speech-api/#dom-speechsynthesisutterance-volume
    float volume() const { return m_volume; }
    void set_volume(float volume) { m_volume = volume; }

    // https://wicg.github.io/speech-api/#dom-speechsynthesisutterance-rate
    float rate() const { return m_rate; }
    void set_rate(float rate) { m_rate = rate; }

    // https://wicg.github.io/speech-api/#dom-speechsynthesisutterance-pitch
    float pitch() const { return m_pitch; }
    void set_pitch(float pitch) { m_pitch = pitch; }

#undef __ENUMERATE
#define __ENUMERATE(attribute_name, event_name)               \
    void set_##attribute_name(GC::Ptr<WebIDL::CallbackType>); \
    GC::Ptr<WebIDL::CallbackType> attribute_name();
    ENUMERATE_SPEECH_SYNTHESIS_UTTERANCE_EVENT_HANDLERS(__ENUMERATE)
#undef __ENUMERATE

private:
    SpeechSynthesisUtterance(JS::Realm&, String const& text);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    String m_text;
    String m_lang;
    GC::Ptr<SpeechSynthesisVoice> m_voice;
    float m_volume { 1.f };
    float m_rate { 1.f };
    float m_pitch { 1.f };
};

}
