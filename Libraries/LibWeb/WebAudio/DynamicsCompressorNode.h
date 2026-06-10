/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/DynamicsCompressorNode.h>
#include <LibWeb/WebAudio/AudioNode.h>

namespace Web::WebAudio {

using DynamicsCompressorOptions = Bindings::DynamicsCompressorOptions;

// https://webaudio.github.io/web-audio-api/#DynamicsCompressorNode
class DynamicsCompressorNode : public AudioNode {
    WEB_WRAPPABLE(DynamicsCompressorNode, AudioNode);
    GC_DECLARE_ALLOCATOR(DynamicsCompressorNode);

public:
    virtual ~DynamicsCompressorNode() override;

    static WebIDL::ExceptionOr<GC::Ref<DynamicsCompressorNode>> create(GC::Ref<BaseAudioContext>, DynamicsCompressorOptions const& = {});
    static WebIDL::ExceptionOr<GC::Ref<DynamicsCompressorNode>> create_for_constructor(GC::Ref<BaseAudioContext>, DynamicsCompressorOptions const& = {});

    WebIDL::UnsignedLong number_of_inputs() override { return 1; }
    WebIDL::UnsignedLong number_of_outputs() override { return 1; }

    GC::Ref<AudioParam const> threshold() const { return m_threshold; }
    GC::Ref<AudioParam const> knee() const { return m_knee; }
    GC::Ref<AudioParam const> ratio() const { return m_ratio; }
    GC::Ref<AudioParam const> attack() const { return m_attack; }
    GC::Ref<AudioParam const> release() const { return m_release; }
    float reduction() const { return m_reduction; }

    WebIDL::ExceptionOr<void> set_channel_count_mode(ChannelCountMode) override;
    WebIDL::ExceptionOr<void> set_channel_count(WebIDL::UnsignedLong) override;

protected:
    DynamicsCompressorNode(GC::Ref<BaseAudioContext>, DynamicsCompressorOptions const& = {});
    virtual void visit_edges(Cell::Visitor&) override;

private:
    // https://webaudio.github.io/web-audio-api/#dom-dynamicscompressornode-threshold
    GC::Ref<AudioParam> m_threshold;

    // https://webaudio.github.io/web-audio-api/#dom-dynamicscompressornode-knee
    GC::Ref<AudioParam> m_knee;

    // https://webaudio.github.io/web-audio-api/#dom-dynamicscompressornode-ratio
    GC::Ref<AudioParam> m_ratio;

    // https://webaudio.github.io/web-audio-api/#dom-dynamicscompressornode-attack
    GC::Ref<AudioParam> m_attack;

    // https://webaudio.github.io/web-audio-api/#dom-dynamicscompressornode-release
    GC::Ref<AudioParam> m_release;

    // https://webaudio.github.io/web-audio-api/#dom-dynamicscompressornode-internal-reduction-slot
    float m_reduction { 0 }; // [[internal reduction]]
};

}
