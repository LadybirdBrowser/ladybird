/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/GainNode.h>
#include <LibWeb/WebAudio/AudioNode.h>

namespace Web::WebAudio {

using GainOptions = Bindings::GainOptions;

// https://webaudio.github.io/web-audio-api/#GainNode
class GainNode : public AudioNode {
    WEB_WRAPPABLE(GainNode, AudioNode);
    GC_DECLARE_ALLOCATOR(GainNode);

public:
    virtual ~GainNode() override;

    static WebIDL::ExceptionOr<GC::Ref<GainNode>> create(GC::Ref<BaseAudioContext>, GainOptions const& = {});
    static WebIDL::ExceptionOr<GC::Ref<GainNode>> create_for_constructor(GC::Ref<BaseAudioContext>, GainOptions const& = {});

    WebIDL::UnsignedLong number_of_inputs() override { return 1; }
    WebIDL::UnsignedLong number_of_outputs() override { return 1; }

    GC::Ref<AudioParam const> gain() const { return m_gain; }

protected:
    GainNode(GC::Ref<BaseAudioContext>, GainOptions const& = {});
    virtual void visit_edges(Cell::Visitor&) override;

private:
    // https://webaudio.github.io/web-audio-api/#dom-gainnode-gain
    GC::Ref<AudioParam> m_gain;
};

}
