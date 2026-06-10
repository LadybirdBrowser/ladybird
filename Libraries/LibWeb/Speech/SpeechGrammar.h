/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Speech {

class SpeechGrammar final : public Bindings::Wrappable {
    WEB_WRAPPABLE(SpeechGrammar, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(SpeechGrammar);

public:
    static GC::Ref<SpeechGrammar> create();
    virtual ~SpeechGrammar() override;

    // https://wicg.github.io/speech-api/#dom-speechgrammar-src
    String const& src() const { return m_src; }
    void set_src(String const& src) { m_src = src; }

    // https://wicg.github.io/speech-api/#dom-speechgrammar-weight
    float weight() const { return m_weight; }
    void set_weight(float weight) { m_weight = weight; }

private:
    explicit SpeechGrammar();

    String m_src;
    float m_weight { 1.f };
};

}
