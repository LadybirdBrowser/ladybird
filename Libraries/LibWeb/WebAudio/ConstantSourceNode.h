/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/WebAudio/AudioScheduledSourceNode.h>

namespace Web::Bindings {

struct ConstantSourceOptions;

}

namespace Web::WebAudio {

// https://webaudio.github.io/web-audio-api/#ChannelMergerNode
class ConstantSourceNode final : public AudioScheduledSourceNode {
    WEB_WRAPPABLE(ConstantSourceNode, AudioScheduledSourceNode);
    GC_DECLARE_ALLOCATOR(ConstantSourceNode);

public:
    virtual ~ConstantSourceNode() override;

    static WebIDL::ExceptionOr<GC::Ref<ConstantSourceNode>> create(GC::Ref<BaseAudioContext>, float offset = 1);
    static WebIDL::ExceptionOr<GC::Ref<ConstantSourceNode>> create_for_constructor(GC::Ref<BaseAudioContext>, Bindings::ConstantSourceOptions const&);
    static WebIDL::ExceptionOr<GC::Ref<ConstantSourceNode>> create_for_constructor(GC::Ref<BaseAudioContext>, float offset = 1);

    virtual WebIDL::UnsignedLong number_of_inputs() override { return 0; }
    virtual WebIDL::UnsignedLong number_of_outputs() override { return 1; }

    GC::Ref<AudioParam const> offset() const { return m_offset; }

private:
    ConstantSourceNode(GC::Ref<BaseAudioContext>, float offset);
    virtual void visit_edges(Cell::Visitor&) override;

    // https://webaudio.github.io/web-audio-api/#dom-constantsourcenode-offset
    GC::Ref<AudioParam> m_offset;
};

}
