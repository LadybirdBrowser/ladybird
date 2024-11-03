/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/WebAudio/AudioNode.h>

namespace Web::WebAudio {

// https://webaudio.github.io/web-audio-api/#DynamicsCompressorOptions
struct DynamicsCompressorOptions : AudioNodeOptions {
    float attack { 0.003 };
    float knee { 30 };
    float ratio { 12 };
    float release { 0.25 };
    float threshold { -24 };
};

// https://webaudio.github.io/web-audio-api/#DynamicsCompressorNode
class DynamicsCompressorNode : public AudioNode {
    WEB_PLATFORM_OBJECT(DynamicsCompressorNode, AudioNode);
    JS_DECLARE_ALLOCATOR(DynamicsCompressorNode);

public:
    virtual ~DynamicsCompressorNode() override;

    static WebIDL::ExceptionOr<JS::NonnullGCPtr<DynamicsCompressorNode>> create(JS::Realm&, JS::NonnullGCPtr<BaseAudioContext>, DynamicsCompressorOptions const& = {});
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<DynamicsCompressorNode>> construct_impl(JS::Realm&, JS::NonnullGCPtr<BaseAudioContext>, DynamicsCompressorOptions const& = {});

    WebIDL::UnsignedLong number_of_inputs() override { return 1; }
    WebIDL::UnsignedLong number_of_outputs() override { return 1; }

    JS::NonnullGCPtr<AudioParam const> threshold() const { return m_threshold; }
    JS::NonnullGCPtr<AudioParam const> knee() const { return m_knee; }
    JS::NonnullGCPtr<AudioParam const> ratio() const { return m_ratio; }
    JS::NonnullGCPtr<AudioParam const> attack() const { return m_attack; }
    JS::NonnullGCPtr<AudioParam const> release() const { return m_release; }
    float reduction() const { return m_reduction; }

    WebIDL::ExceptionOr<void> set_channel_count_mode(Bindings::ChannelCountMode) override;
    WebIDL::ExceptionOr<void> set_channel_count(WebIDL::UnsignedLong) override;

protected:
    DynamicsCompressorNode(JS::Realm&, JS::NonnullGCPtr<BaseAudioContext>, DynamicsCompressorOptions const& = {});

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

private:
    // https://webaudio.github.io/web-audio-api/#dom-dynamicscompressornode-threshold
    JS::NonnullGCPtr<AudioParam> m_threshold;

    // https://webaudio.github.io/web-audio-api/#dom-dynamicscompressornode-knee
    JS::NonnullGCPtr<AudioParam> m_knee;

    // https://webaudio.github.io/web-audio-api/#dom-dynamicscompressornode-ratio
    JS::NonnullGCPtr<AudioParam> m_ratio;

    // https://webaudio.github.io/web-audio-api/#dom-dynamicscompressornode-attack
    JS::NonnullGCPtr<AudioParam> m_attack;

    // https://webaudio.github.io/web-audio-api/#dom-dynamicscompressornode-release
    JS::NonnullGCPtr<AudioParam> m_release;

    // https://webaudio.github.io/web-audio-api/#dom-dynamicscompressornode-internal-reduction-slot
    float m_reduction { 0 }; // [[internal reduction]]
};

}
