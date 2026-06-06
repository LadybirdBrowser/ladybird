/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Speech/SpeechSynthesisVoice.h>

namespace Web::Speech {

GC_DEFINE_ALLOCATOR(SpeechSynthesisVoice);

GC::Ref<SpeechSynthesisVoice> SpeechSynthesisVoice::create(JS::Realm& realm)
{
    return realm.create<SpeechSynthesisVoice>(realm);
}

SpeechSynthesisVoice::SpeechSynthesisVoice(JS::Realm& realm)
    : Wrappable(realm)
{
}

SpeechSynthesisVoice::~SpeechSynthesisVoice() = default;

}
