/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Speech/SpeechRecognitionResult.h>

namespace Web::Speech {

class SpeechRecognitionResultList final : public Bindings::Wrappable {
    WEB_WRAPPABLE(SpeechRecognitionResultList, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(SpeechRecognitionResultList);

public:
    [[nodiscard]] static GC::Ref<SpeechRecognitionResultList> create();
    virtual ~SpeechRecognitionResultList() override;

    // https://wicg.github.io/speech-api/#dom-speechrecognitionresultlist-length
    size_t length() const { return m_results.size(); }

    // https://wicg.github.io/speech-api/#dom-speechrecognitionresultlist-item
    GC::Ptr<SpeechRecognitionResult> item(size_t index) const;

private:
    explicit SpeechRecognitionResultList();

    virtual void visit_edges(GC::Cell::Visitor&) override;

    Vector<GC::Ref<SpeechRecognitionResult>> m_results;
};

}
