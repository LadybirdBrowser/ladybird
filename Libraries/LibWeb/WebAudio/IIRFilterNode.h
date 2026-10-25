/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibWeb/Bindings/IIRFilterNode.h>
#include <LibWeb/WebAudio/AudioNode.h>

namespace Web::WebAudio {

using IIRFilterOptions = Bindings::IIRFilterOptions;

// https://webaudio.github.io/web-audio-api/#IIRFilterNode
class IIRFilterNode final : public AudioNode {
    WEB_WRAPPABLE(IIRFilterNode, AudioNode);
    GC_DECLARE_ALLOCATOR(IIRFilterNode);

public:
    virtual ~IIRFilterNode() override;

    static WebIDL::ExceptionOr<GC::Ref<IIRFilterNode>> create(GC::Ref<BaseAudioContext>, IIRFilterOptions const&);
    static WebIDL::ExceptionOr<GC::Ref<IIRFilterNode>> create_for_constructor(GC::Ref<BaseAudioContext>, IIRFilterOptions const&);

    WebIDL::UnsignedLong number_of_inputs() override { return 1; }
    WebIDL::UnsignedLong number_of_outputs() override { return 1; }

    WebIDL::ExceptionOr<void> get_frequency_response(GC::Ref<JS::Float32Array> frequency_hz, GC::Ref<JS::Float32Array> mag_response, GC::Ref<JS::Float32Array> phase_response);

private:
    IIRFilterNode(GC::Ref<BaseAudioContext>, Vector<double> feedforward, Vector<double> feedback);

    // The coefficients, divided by the first feedback coefficient.
    Vector<double> m_feedforward;
    Vector<double> m_feedback;
};

}
