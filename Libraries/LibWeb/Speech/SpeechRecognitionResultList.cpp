/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SpeechRecognitionResultListPrototype.h>
#include <LibWeb/Speech/SpeechRecognitionResultList.h>

namespace Web::Speech {

GC_DEFINE_ALLOCATOR(SpeechRecognitionResultList);

GC::Ref<SpeechRecognitionResultList> SpeechRecognitionResultList::create(JS::Realm& realm)
{
    return realm.create<SpeechRecognitionResultList>(realm);
}

SpeechRecognitionResultList::SpeechRecognitionResultList(JS::Realm& realm)
    : Bindings::PlatformObject(realm)
{
}

SpeechRecognitionResultList::~SpeechRecognitionResultList() = default;

void SpeechRecognitionResultList::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SpeechRecognitionResultList);
    Base::initialize(realm);
}

void SpeechRecognitionResultList::visit_edges(Cell::Visitor& visitor)
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
