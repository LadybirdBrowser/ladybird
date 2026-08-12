/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::WebAudio {

// https://webaudio.github.io/web-audio-api/#AudioWorkletProcessor
class WEB_API AudioWorkletProcessor final : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(AudioWorkletProcessor, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(AudioWorkletProcessor);

public:
    // https://webaudio.github.io/web-audio-api/#dom-audioworkletprocessor-audioworkletprocessor
    // Runs when `super()` executes in a registered processor subclass during node instantiation.
    static WebIDL::ExceptionOr<GC::Ref<AudioWorkletProcessor>> construct_impl(JS::Realm&);

    virtual ~AudioWorkletProcessor() override;

    GC::Ref<HTML::MessagePort> port() const { return m_port; }

private:
    explicit AudioWorkletProcessor(GC::Ref<HTML::MessagePort>);

    // Ensures wrappers for this processor are created in the worklet realm, not the page realm.
    virtual GC::Ptr<Bindings::Wrappable> relevant_global_impl() const override;

    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ref<HTML::MessagePort> m_port;
};

}
