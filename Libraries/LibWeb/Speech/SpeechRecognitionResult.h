/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/SpeechRecognitionResult.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Speech/SpeechRecognitionAlternative.h>

namespace Web::Speech {

class SpeechRecognitionResult final : public Bindings::Wrappable {
    WEB_WRAPPABLE(SpeechRecognitionResult, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(SpeechRecognitionResult);

public:
    [[nodiscard]] static GC::Ref<SpeechRecognitionResult> create(JS::Realm&);
    virtual ~SpeechRecognitionResult() override;

    // https://wicg.github.io/speech-api/#dom-speechrecognitionresult-length
    size_t length() const { return m_alternatives.size(); }

    // https://wicg.github.io/speech-api/#dom-speechrecognitionresult-item
    GC::Ptr<SpeechRecognitionAlternative> item(size_t index) const;

    // https://wicg.github.io/speech-api/#dom-speechrecognitionresult-isfinal
    bool is_final() const { return m_is_final; }

private:
    explicit SpeechRecognitionResult(JS::Realm&);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    Vector<GC::Ref<SpeechRecognitionAlternative>> m_alternatives;
    bool m_is_final { false };
};

}
