/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Speech {

class SpeechRecognitionPhrase final : public Bindings::Wrappable {
    WEB_WRAPPABLE(SpeechRecognitionPhrase, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(SpeechRecognitionPhrase);

public:
    static GC::Ref<SpeechRecognitionPhrase> create(String const& phrase, float boost = 1.f);
    virtual ~SpeechRecognitionPhrase() override;

    // https://wicg.github.io/speech-api/#dom-speechrecognitionphrase-phrase
    String const& phrase() const { return m_phrase; }

    // https://wicg.github.io/speech-api/#dom-speechrecognitionphrase-boost
    float boost() const { return m_boost; }

private:
    SpeechRecognitionPhrase(String const& phrase, float boost);

    String m_phrase;
    float m_boost { 1.f };
};

}
