/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Speech/SpeechRecognitionResult.h>

namespace Web::Speech {

GC_DEFINE_ALLOCATOR(SpeechRecognitionResult);

GC::Ref<SpeechRecognitionResult> SpeechRecognitionResult::create()
{
    return GC::Heap::the().allocate<SpeechRecognitionResult>();
}

SpeechRecognitionResult::SpeechRecognitionResult()
{
}

SpeechRecognitionResult::~SpeechRecognitionResult() = default;

void SpeechRecognitionResult::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_alternatives);
}

// https://wicg.github.io/speech-api/#dom-speechrecognitionresult-item
GC::Ptr<SpeechRecognitionAlternative> SpeechRecognitionResult::item(size_t index) const
{
    if (index >= m_alternatives.size())
        return nullptr;
    return m_alternatives[index];
}

}
