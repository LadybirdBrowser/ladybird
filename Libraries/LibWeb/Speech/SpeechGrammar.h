/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Speech {

class SpeechGrammar final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(SpeechGrammar, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(SpeechGrammar);

public:
    static WebIDL::ExceptionOr<GC::Ref<SpeechGrammar>> construct_impl(JS::Realm&);
    virtual ~SpeechGrammar() override;

    // https://wicg.github.io/speech-api/#dom-speechgrammar-src
    String const& src() const { return m_src; }
    void set_src(String const& src) { m_src = src; }

    // https://wicg.github.io/speech-api/#dom-speechgrammar-weight
    float weight() const { return m_weight; }
    void set_weight(float weight) { m_weight = weight; }

private:
    explicit SpeechGrammar(JS::Realm&);

    virtual void initialize(JS::Realm&) override;

    String m_src;
    float m_weight { 1.f };
};

}
