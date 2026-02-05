/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SpeechRecognitionResultPrototype.h>
#include <LibWeb/Speech/SpeechRecognitionResult.h>

namespace Web::Speech {

GC_DEFINE_ALLOCATOR(SpeechRecognitionResult);

GC::Ref<SpeechRecognitionResult> SpeechRecognitionResult::create(JS::Realm& realm)
{
    return realm.create<SpeechRecognitionResult>(realm);
}

SpeechRecognitionResult::SpeechRecognitionResult(JS::Realm& realm)
    : Bindings::PlatformObject(realm)
{
}

SpeechRecognitionResult::~SpeechRecognitionResult() = default;

void SpeechRecognitionResult::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SpeechRecognitionResult);
    Base::initialize(realm);
}

void SpeechRecognitionResult::visit_edges(Cell::Visitor& visitor)
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
