/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SpeechRecognitionPhrasePrototype.h>
#include <LibWeb/Speech/SpeechRecognitionPhrase.h>

namespace Web::Speech {

GC_DEFINE_ALLOCATOR(SpeechRecognitionPhrase);

WebIDL::ExceptionOr<GC::Ref<SpeechRecognitionPhrase>> SpeechRecognitionPhrase::construct_impl(JS::Realm& realm, String const& phrase, float boost)
{
    return realm.create<SpeechRecognitionPhrase>(realm, phrase, boost);
}

SpeechRecognitionPhrase::SpeechRecognitionPhrase(JS::Realm& realm, String const& phrase, float boost)
    : Bindings::PlatformObject(realm)
    , m_phrase(phrase)
    , m_boost(boost)
{
}

SpeechRecognitionPhrase::~SpeechRecognitionPhrase() = default;

void SpeechRecognitionPhrase::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SpeechRecognitionPhrase);
    Base::initialize(realm);
}

}
