/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Speech/SpeechRecognitionPhrase.h>

namespace Web::Speech {

GC_DEFINE_ALLOCATOR(SpeechRecognitionPhrase);

GC::Ref<SpeechRecognitionPhrase> SpeechRecognitionPhrase::create(String const& phrase, float boost)
{
    return GC::Heap::the().allocate<SpeechRecognitionPhrase>(phrase, boost);
}

SpeechRecognitionPhrase::SpeechRecognitionPhrase(String const& phrase, float boost)
    : m_phrase(phrase)
    , m_boost(boost)
{
}

SpeechRecognitionPhrase::~SpeechRecognitionPhrase() = default;

}
