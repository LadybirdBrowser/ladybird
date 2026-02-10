/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/Forward.h>

namespace Web::Speech {

class SpeechSynthesis final : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(SpeechSynthesis, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(SpeechSynthesis);

public:
    [[nodiscard]] static GC::Ref<SpeechSynthesis> create(JS::Realm&);
    virtual ~SpeechSynthesis() override;

    // https://wicg.github.io/speech-api/#dom-speechsynthesis-pending
    bool pending() const { return m_pending; }

    // https://wicg.github.io/speech-api/#dom-speechsynthesis-speaking
    bool speaking() const { return m_speaking; }

    // https://wicg.github.io/speech-api/#dom-speechsynthesis-paused
    bool paused() const { return m_paused; }

    void set_onvoiceschanged(GC::Ptr<WebIDL::CallbackType>);
    GC::Ptr<WebIDL::CallbackType> onvoiceschanged();

    void cancel();

    Vector<GC::Ref<SpeechSynthesisVoice>> const& get_voices() const;

private:
    explicit SpeechSynthesis(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    bool m_pending { false };
    bool m_speaking { false };
    bool m_paused { false };
    Vector<GC::Ref<SpeechSynthesisVoice>> m_voices;
};

}
