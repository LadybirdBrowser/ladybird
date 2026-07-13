/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Forward.h>

namespace Web::Speech {

class SpeechSynthesisVoice final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(SpeechSynthesisVoice, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(SpeechSynthesisVoice);

public:
    [[nodiscard]] static GC::Ref<SpeechSynthesisVoice> create(JS::Realm&);
    virtual ~SpeechSynthesisVoice() override;

    // https://wicg.github.io/speech-api/#dom-speechsynthesisvoice-voiceuri
    Utf16String const& voice_uri() const { return m_voice_uri; }

    // https://wicg.github.io/speech-api/#dom-speechsynthesisvoice-name
    Utf16String const& name() const { return m_name; }

    // https://wicg.github.io/speech-api/#dom-speechsynthesisvoice-lang
    Utf16String const& lang() const { return m_lang; }

    // https://wicg.github.io/speech-api/#dom-speechsynthesisvoice-localservice
    bool local_service() const { return m_local_service; }

    // https://wicg.github.io/speech-api/#dom-speechsynthesisvoice-default
    bool is_default() const { return m_default; }

private:
    explicit SpeechSynthesisVoice(JS::Realm&);

    virtual void initialize(JS::Realm&) override;

    Utf16String m_voice_uri;
    Utf16String m_name;
    Utf16String m_lang;
    bool m_local_service { false };
    bool m_default { false };
};

}
