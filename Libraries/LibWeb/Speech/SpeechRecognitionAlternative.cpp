/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SpeechRecognitionAlternativePrototype.h>
#include <LibWeb/Speech/SpeechRecognitionAlternative.h>

namespace Web::Speech {

GC_DEFINE_ALLOCATOR(SpeechRecognitionAlternative);

GC::Ref<SpeechRecognitionAlternative> SpeechRecognitionAlternative::create(JS::Realm& realm)
{
    return realm.create<SpeechRecognitionAlternative>(realm);
}

SpeechRecognitionAlternative::SpeechRecognitionAlternative(JS::Realm& realm)
    : Bindings::PlatformObject(realm)
{
}

SpeechRecognitionAlternative::~SpeechRecognitionAlternative() = default;

void SpeechRecognitionAlternative::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SpeechRecognitionAlternative);
    Base::initialize(realm);
}

}
