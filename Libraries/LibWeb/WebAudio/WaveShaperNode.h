/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/WaveShaperNodePrototype.h>
#include <LibWeb/WebAudio/AudioNode.h>
#include <LibWeb/WebIDL/Buffers.h>

namespace Web::WebAudio {

// https://webaudio.github.io/web-audio-api/#WaveShaperOptions
struct WaveShaperOptions : AudioNodeOptions {
    Optional<Vector<float>> curve;
    Bindings::OverSampleType oversample { Bindings::OverSampleType::None };
};

// https://webaudio.github.io/web-audio-api/#WaveShaperNode
class WaveShaperNode final : public AudioNode {
    WEB_PLATFORM_OBJECT(WaveShaperNode, AudioNode);
    GC_DECLARE_ALLOCATOR(WaveShaperNode);

public:
    virtual ~WaveShaperNode() override;

    static WebIDL::ExceptionOr<GC::Ref<WaveShaperNode>> create(JS::Realm&, GC::Ref<BaseAudioContext>, WaveShaperOptions const& = {});
    static WebIDL::ExceptionOr<GC::Ref<WaveShaperNode>> construct_impl(JS::Realm&, GC::Ref<BaseAudioContext>, WaveShaperOptions const& = {});

    WebIDL::UnsignedLong number_of_inputs() override { return 1; }
    WebIDL::UnsignedLong number_of_outputs() override { return 1; }

    GC::Ptr<JS::Float32Array> curve() const { return m_curve; }
    WebIDL::ExceptionOr<void> set_curve(Optional<GC::Root<WebIDL::BufferSource>> const&);

    Bindings::OverSampleType oversample() const { return m_oversample; }
    void set_oversample(Bindings::OverSampleType);

protected:
    WaveShaperNode(JS::Realm&, GC::Ref<BaseAudioContext>, WaveShaperOptions const& = {});

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

private:
    GC::Ptr<JS::Float32Array> m_curve;
    Bindings::OverSampleType m_oversample { Bindings::OverSampleType::None };
};

}
