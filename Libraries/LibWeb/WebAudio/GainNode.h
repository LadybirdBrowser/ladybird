/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/GainNodePrototype.h>
#include <LibWeb/WebAudio/AudioNode.h>

namespace Web::WebAudio {

// https://webaudio.github.io/web-audio-api/#GainOptions
struct GainOptions : AudioNodeOptions {
    float gain { 1.0 };
};

// https://webaudio.github.io/web-audio-api/#GainNode
class GainNode : public AudioNode {
    WEB_PLATFORM_OBJECT(GainNode, AudioNode);
    GC_DECLARE_ALLOCATOR(GainNode);

public:
    virtual ~GainNode() override;

    static WebIDL::ExceptionOr<GC::Ref<GainNode>> create(JS::Realm&, GC::Ref<BaseAudioContext>, GainOptions const& = {});
    static WebIDL::ExceptionOr<GC::Ref<GainNode>> construct_impl(JS::Realm&, GC::Ref<BaseAudioContext>, GainOptions const& = {});

    WebIDL::UnsignedLong number_of_inputs() override { return 1; }
    WebIDL::UnsignedLong number_of_outputs() override { return 1; }

    GC::Ref<AudioParam const> gain() const { return m_gain; }

protected:
    GainNode(JS::Realm&, GC::Ref<BaseAudioContext>, GainOptions const& = {});

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

private:
    // https://webaudio.github.io/web-audio-api/#dom-gainnode-gain
    GC::Ref<AudioParam> m_gain;
};

}
