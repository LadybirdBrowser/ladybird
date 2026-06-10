/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Speech/SpeechRecognitionResultList.h>

namespace Web::Speech {

GC_DEFINE_ALLOCATOR(SpeechRecognitionResultList);

GC::Ref<SpeechRecognitionResultList> SpeechRecognitionResultList::create()
{
    return GC::Heap::the().allocate<SpeechRecognitionResultList>();
}

SpeechRecognitionResultList::SpeechRecognitionResultList()
{
}

SpeechRecognitionResultList::~SpeechRecognitionResultList() = default;

void SpeechRecognitionResultList::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_results);
}

// https://wicg.github.io/speech-api/#dom-speechrecognitionresultlist-item
GC::Ptr<SpeechRecognitionResult> SpeechRecognitionResultList::item(size_t index) const
{
    if (index >= m_results.size())
        return nullptr;
    return m_results[index];
}

}
