/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Speech/SpeechGrammar.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Speech {

GC_DEFINE_ALLOCATOR(SpeechGrammar);

GC::Ref<SpeechGrammar> SpeechGrammar::create()
{
    return GC::Heap::the().allocate<SpeechGrammar>();
}

SpeechGrammar::SpeechGrammar()
{
}

SpeechGrammar::~SpeechGrammar() = default;

}
