/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Speech/SpeechRecognitionAlternative.h>

namespace Web::Speech {

GC_DEFINE_ALLOCATOR(SpeechRecognitionAlternative);

GC::Ref<SpeechRecognitionAlternative> SpeechRecognitionAlternative::create(JS::Realm& realm)
{
    return realm.create<SpeechRecognitionAlternative>(realm);
}

SpeechRecognitionAlternative::SpeechRecognitionAlternative(JS::Realm& realm)
    : Wrappable(realm)
{
}

SpeechRecognitionAlternative::~SpeechRecognitionAlternative() = default;

}
