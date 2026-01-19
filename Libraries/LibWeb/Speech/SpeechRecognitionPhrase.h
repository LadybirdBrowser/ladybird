/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Speech {

class SpeechRecognitionPhrase final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(SpeechRecognitionPhrase, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(SpeechRecognitionPhrase);

public:
    static WebIDL::ExceptionOr<GC::Ref<SpeechRecognitionPhrase>> construct_impl(JS::Realm&, String const& phrase, float boost = 1.f);
    virtual ~SpeechRecognitionPhrase() override;

    // https://wicg.github.io/speech-api/#dom-speechrecognitionphrase-phrase
    String const& phrase() const { return m_phrase; }

    // https://wicg.github.io/speech-api/#dom-speechrecognitionphrase-boost
    float boost() const { return m_boost; }

private:
    SpeechRecognitionPhrase(JS::Realm&, String const& phrase, float boost);

    virtual void initialize(JS::Realm&) override;

    String m_phrase;
    float m_boost { 1.f };
};

}
