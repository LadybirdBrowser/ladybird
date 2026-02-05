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

namespace Web::Speech {

class SpeechRecognitionAlternative final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(SpeechRecognitionAlternative, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(SpeechRecognitionAlternative);

public:
    [[nodiscard]] static GC::Ref<SpeechRecognitionAlternative> create(JS::Realm&);
    virtual ~SpeechRecognitionAlternative() override;

    // https://wicg.github.io/speech-api/#dom-speechrecognitionalternative-transcript
    String const& transcript() const { return m_transcript; }

    // https://wicg.github.io/speech-api/#dom-speechrecognitionalternative-confidence
    float confidence() const { return m_confidence; }

private:
    explicit SpeechRecognitionAlternative(JS::Realm&);

    virtual void initialize(JS::Realm&) override;

    String m_transcript;
    float m_confidence { 0.f };
};

}
