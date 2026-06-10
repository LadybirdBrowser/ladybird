/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Speech/SpeechGrammarList.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Speech {

GC_DEFINE_ALLOCATOR(SpeechGrammarList);

GC::Ref<SpeechGrammarList> SpeechGrammarList::create()
{
    return GC::Heap::the().allocate<SpeechGrammarList>();
}

SpeechGrammarList::SpeechGrammarList()
{
}

SpeechGrammarList::~SpeechGrammarList() = default;

void SpeechGrammarList::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_grammars);
}

// https://wicg.github.io/speech-api/#dom-speechgrammarlist-item
GC::Ptr<SpeechGrammar> SpeechGrammarList::item(size_t index) const
{
    if (index >= m_grammars.size())
        return nullptr;
    return m_grammars[index];
}

}
