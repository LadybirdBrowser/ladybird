/*
 * Copyright (c) 2024, Bar Yemini <bar.ye651@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/BiquadFilterNodePrototype.h>
#include <LibWeb/WebAudio/AudioNode.h>
#include <LibWeb/WebAudio/AudioParam.h>

namespace Web::WebAudio {

// https://webaudio.github.io/web-audio-api/#BiquadFilterOptions
struct BiquadFilterOptions : AudioNodeOptions {
    Bindings::BiquadFilterType type { Bindings::BiquadFilterType::Lowpass };
    float q { 1 };
    float detune { 0 };
    float frequency { 350 };
    float gain { 0 };
};

// https://webaudio.github.io/web-audio-api/#BiquadFilterNode
class BiquadFilterNode : public AudioNode {
    WEB_PLATFORM_OBJECT(BiquadFilterNode, AudioNode);
    JS_DECLARE_ALLOCATOR(BiquadFilterNode);

public:
    virtual ~BiquadFilterNode() override;

    WebIDL::UnsignedLong number_of_inputs() override { return 1; }
    WebIDL::UnsignedLong number_of_outputs() override { return 1; }

    void set_type(Bindings::BiquadFilterType);
    Bindings::BiquadFilterType type() const;
    JS::NonnullGCPtr<AudioParam> frequency() const;
    JS::NonnullGCPtr<AudioParam> detune() const;
    JS::NonnullGCPtr<AudioParam> q() const;
    JS::NonnullGCPtr<AudioParam> gain() const;
    WebIDL::ExceptionOr<void> get_frequency_response(JS::Handle<WebIDL::BufferSource> const&, JS::Handle<WebIDL::BufferSource> const&, JS::Handle<WebIDL::BufferSource> const&);

    static WebIDL::ExceptionOr<JS::NonnullGCPtr<BiquadFilterNode>> create(JS::Realm&, JS::NonnullGCPtr<BaseAudioContext>, BiquadFilterOptions const& = {});
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<BiquadFilterNode>> construct_impl(JS::Realm&, JS::NonnullGCPtr<BaseAudioContext>, BiquadFilterOptions const& = {});

protected:
    BiquadFilterNode(JS::Realm&, JS::NonnullGCPtr<BaseAudioContext>, BiquadFilterOptions const& = {});

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

private:
    Bindings::BiquadFilterType m_type { Bindings::BiquadFilterType::Lowpass };
    JS::NonnullGCPtr<AudioParam> m_frequency;
    JS::NonnullGCPtr<AudioParam> m_detune;
    JS::NonnullGCPtr<AudioParam> m_q;
    JS::NonnullGCPtr<AudioParam> m_gain;
};

}
