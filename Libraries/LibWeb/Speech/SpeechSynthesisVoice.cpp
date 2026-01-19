/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SpeechSynthesisVoicePrototype.h>
#include <LibWeb/Speech/SpeechSynthesisVoice.h>

namespace Web::Speech {

GC_DEFINE_ALLOCATOR(SpeechSynthesisVoice);

GC::Ref<SpeechSynthesisVoice> SpeechSynthesisVoice::create(JS::Realm& realm)
{
    return realm.create<SpeechSynthesisVoice>(realm);
}

SpeechSynthesisVoice::SpeechSynthesisVoice(JS::Realm& realm)
    : Bindings::PlatformObject(realm)
{
}

SpeechSynthesisVoice::~SpeechSynthesisVoice() = default;

void SpeechSynthesisVoice::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SpeechSynthesisVoice);
    Base::initialize(realm);
}

}
