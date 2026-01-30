/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/AudioWorkletProcessorPrototype.h>
#include <LibWeb/Forward.h>

namespace Web::WebAudio {

class AudioWorkletProcessor final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(AudioWorkletProcessor, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(AudioWorkletProcessor);

public:
    virtual ~AudioWorkletProcessor() override;

    static WebIDL::ExceptionOr<GC::Ref<AudioWorkletProcessor>> construct_impl(JS::Realm&);

    GC::Ref<HTML::MessagePort> port() const { return m_port; }

private:
    explicit AudioWorkletProcessor(JS::Realm&, GC::Ref<HTML::MessagePort>);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ref<HTML::MessagePort> m_port;
};

}
