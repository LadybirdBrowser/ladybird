/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibWeb/Bindings/IIRFilterNodePrototype.h>
#include <LibWeb/WebAudio/AudioNode.h>

namespace Web::WebAudio {

// https://webaudio.github.io/web-audio-api/#IIRFilterOptions
struct IIRFilterOptions : AudioNodeOptions {
    Vector<double> feedforward;
    Vector<double> feedback;
};

// https://webaudio.github.io/web-audio-api/#IIRFilterNode
class IIRFilterNode : public AudioNode {
    WEB_PLATFORM_OBJECT(IIRFilterNode, AudioNode);
    GC_DECLARE_ALLOCATOR(IIRFilterNode);

public:
    virtual ~IIRFilterNode() override;

    WebIDL::UnsignedLong number_of_inputs() override { return 1; }
    WebIDL::UnsignedLong number_of_outputs() override { return 1; }

    WebIDL::ExceptionOr<void> get_frequency_response(GC::Root<WebIDL::BufferSource> const& frequency_hz, GC::Root<WebIDL::BufferSource> const& mag_response, GC::Root<WebIDL::BufferSource> const& phase_response);

    ReadonlySpan<double> feedforward() const { return m_feedforward.span(); }
    ReadonlySpan<double> feedback() const { return m_feedback.span(); }

    static WebIDL::ExceptionOr<GC::Ref<IIRFilterNode>> create(JS::Realm&, GC::Ref<BaseAudioContext>, IIRFilterOptions const&);
    static WebIDL::ExceptionOr<GC::Ref<IIRFilterNode>> construct_impl(JS::Realm&, GC::Ref<BaseAudioContext>, IIRFilterOptions const&);

protected:
    IIRFilterNode(JS::Realm&, GC::Ref<BaseAudioContext>, IIRFilterOptions const&);

    virtual void initialize(JS::Realm&) override;

private:
    Vector<double> m_feedforward;
    Vector<double> m_feedback;
};

}
