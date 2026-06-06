/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/DelayNode.h>
#include <LibWeb/WebAudio/AudioNode.h>

namespace Web::WebAudio {

// https://webaudio.github.io/web-audio-api/#DelayNode
class DelayNode final : public AudioNode {
    WEB_WRAPPABLE(DelayNode, AudioNode);
    GC_DECLARE_ALLOCATOR(DelayNode);

public:
    virtual ~DelayNode() override;

    static WebIDL::ExceptionOr<GC::Ref<DelayNode>> create(GC::Ref<BaseAudioContext>, Bindings::DelayOptions const& = {});
    static WebIDL::ExceptionOr<void> validate_options(Bindings::DelayOptions const&);
    static WebIDL::ExceptionOr<GC::Ref<DelayNode>> construct_impl(GC::Ref<BaseAudioContext>, Bindings::DelayOptions const& = {});

    virtual WebIDL::UnsignedLong number_of_inputs() override { return 1; }
    virtual WebIDL::UnsignedLong number_of_outputs() override { return 1; }

    GC::Ref<AudioParam const> delay_time() const { return m_delay_time; }

private:
    DelayNode(GC::Ref<BaseAudioContext>, Bindings::DelayOptions const&);
    virtual void visit_edges(Cell::Visitor&) override;

    // https://webaudio.github.io/web-audio-api/#dom-delaynode-delaytime
    GC::Ref<AudioParam> m_delay_time;
};

}
