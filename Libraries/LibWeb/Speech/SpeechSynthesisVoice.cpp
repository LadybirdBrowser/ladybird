/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Speech/SpeechSynthesisVoice.h>

namespace Web::Speech {

GC_DEFINE_ALLOCATOR(SpeechSynthesisVoice);

GC::Ref<SpeechSynthesisVoice> SpeechSynthesisVoice::create()
{
    return GC::Heap::the().allocate<SpeechSynthesisVoice>();
}

SpeechSynthesisVoice::SpeechSynthesisVoice()
{
}

SpeechSynthesisVoice::~SpeechSynthesisVoice() = default;

}
