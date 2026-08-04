/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Speech {

class SpeechRecognitionPhrase final : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(SpeechRecognitionPhrase, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(SpeechRecognitionPhrase);

public:
    static GC::Ref<SpeechRecognitionPhrase> create(Utf16String const& phrase, float boost = 1.f);
    virtual ~SpeechRecognitionPhrase() override;

    // https://wicg.github.io/speech-api/#dom-speechrecognitionphrase-phrase
    Utf16String const& phrase() const { return m_phrase; }

    // https://wicg.github.io/speech-api/#dom-speechrecognitionphrase-boost
    float boost() const { return m_boost; }

private:
    SpeechRecognitionPhrase(Utf16String const& phrase, float boost);

    Utf16String m_phrase;
    float m_boost { 1.f };
};

}
