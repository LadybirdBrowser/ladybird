/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Speech/SpeechRecognitionAlternative.h>

namespace Web::Speech {

GC_DEFINE_ALLOCATOR(SpeechRecognitionAlternative);

GC::Ref<SpeechRecognitionAlternative> SpeechRecognitionAlternative::create()
{
    return GC::Heap::the().allocate<SpeechRecognitionAlternative>();
}

SpeechRecognitionAlternative::SpeechRecognitionAlternative()
{
}

SpeechRecognitionAlternative::~SpeechRecognitionAlternative() = default;

}
