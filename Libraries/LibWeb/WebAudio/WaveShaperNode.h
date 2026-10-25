/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/WaveShaperNode.h>
#include <LibWeb/WebAudio/AudioNode.h>
#include <LibWeb/WebAudio/Rendering/AudioData.h>

namespace Web::WebAudio {

using OverSampleType = Bindings::OverSampleType;
using WaveShaperOptions = Bindings::WaveShaperOptions;

// https://webaudio.github.io/web-audio-api/#WaveShaperNode
class WaveShaperNode final : public AudioNode {
    WEB_WRAPPABLE(WaveShaperNode, AudioNode);
    GC_DECLARE_ALLOCATOR(WaveShaperNode);

public:
    virtual ~WaveShaperNode() override;

    static WebIDL::ExceptionOr<GC::Ref<WaveShaperNode>> create(GC::Ref<BaseAudioContext>, WaveShaperOptions const& = {});
    static WebIDL::ExceptionOr<GC::Ref<WaveShaperNode>> create_for_constructor(GC::Ref<BaseAudioContext>, WaveShaperOptions const& = {});

    WebIDL::UnsignedLong number_of_inputs() override { return 1; }
    WebIDL::UnsignedLong number_of_outputs() override { return 1; }

    GC::Ptr<JS::Float32Array> curve() const { return m_curve; }
    WebIDL::ExceptionOr<void> set_curve(GC::Ptr<JS::Float32Array>);

    OverSampleType oversample() const { return m_oversample; }
    void set_oversample(OverSampleType);

private:
    explicit WaveShaperNode(GC::Ref<BaseAudioContext>);

    virtual void visit_edges(Cell::Visitor&) override;

    void send_parameters_to_render_node();

    // https://webaudio.github.io/web-audio-api/#dom-waveshapernode-curve
    GC::Ptr<JS::Float32Array> m_curve;
    // When this attribute is set, an internal copy of the curve is created by the WaveShaperNode.
    RefPtr<Rendering::WaveShaperCurve> m_curve_copy;

    // https://webaudio.github.io/web-audio-api/#dom-waveshapernode-curve-set-slot
    bool m_curve_set { false };

    // https://webaudio.github.io/web-audio-api/#dom-waveshapernode-oversample
    OverSampleType m_oversample { OverSampleType::None };
};

}
