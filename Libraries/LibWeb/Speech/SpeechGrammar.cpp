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

WebIDL::ExceptionOr<GC::Ref<SpeechGrammar>> SpeechGrammar::construct_impl()
{
    return GC::Heap::the().allocate<SpeechGrammar>();
}

SpeechGrammar::SpeechGrammar()
    : Bindings::Wrappable()
{
}

SpeechGrammar::~SpeechGrammar() = default;

}
