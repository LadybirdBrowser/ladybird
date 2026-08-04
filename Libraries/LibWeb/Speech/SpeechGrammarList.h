/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Speech/SpeechGrammar.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Speech {

class SpeechGrammarList final : public Bindings::Wrappable {
    WEB_WRAPPABLE(SpeechGrammarList, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(SpeechGrammarList);

public:
    static GC::Ref<SpeechGrammarList> create();
    virtual ~SpeechGrammarList() override;

    // https://wicg.github.io/speech-api/#dom-speechgrammarlist-length
    size_t length() const { return m_grammars.size(); }

    // https://wicg.github.io/speech-api/#dom-speechgrammarlist-item
    GC::Ptr<SpeechGrammar> item(size_t index) const;

private:
    explicit SpeechGrammarList();

    virtual void visit_edges(GC::Cell::Visitor&) override;

    Vector<GC::Ref<SpeechGrammar>> m_grammars;
};

}
