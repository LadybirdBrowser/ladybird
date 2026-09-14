/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/ConvolverNode.h>
#include <LibWeb/WebAudio/AudioBuffer.h>
#include <LibWeb/WebAudio/AudioNode.h>

namespace Web::WebAudio {

using ConvolverOptions = Bindings::ConvolverOptions;

// https://webaudio.github.io/web-audio-api/#ConvolverNode
class ConvolverNode final : public AudioNode {
    WEB_WRAPPABLE(ConvolverNode, AudioNode);
    GC_DECLARE_ALLOCATOR(ConvolverNode);

public:
    virtual ~ConvolverNode() override;

    static WebIDL::ExceptionOr<GC::Ref<ConvolverNode>> create(GC::Ref<BaseAudioContext>, ConvolverOptions const& = {});
    static WebIDL::ExceptionOr<GC::Ref<ConvolverNode>> create_for_constructor(GC::Ref<BaseAudioContext>, ConvolverOptions const& = {});

    WebIDL::UnsignedLong number_of_inputs() override { return 1; }
    WebIDL::UnsignedLong number_of_outputs() override { return 1; }

    GC::Ptr<AudioBuffer> buffer() const { return m_buffer; }
    WebIDL::ExceptionOr<void> set_buffer(GC::Ptr<AudioBuffer>);

    bool normalize() const { return m_normalize; }
    void set_normalize(bool normalize) { m_normalize = normalize; }

    WebIDL::ExceptionOr<void> set_channel_count(WebIDL::UnsignedLong) override;
    WebIDL::ExceptionOr<void> set_channel_count_mode(ChannelCountMode) override;

private:
    ConvolverNode(GC::Ref<BaseAudioContext>, ConvolverOptions const&);
    virtual void visit_edges(Cell::Visitor&) override;

    // https://webaudio.github.io/web-audio-api/#dom-convolvernode-buffer
    GC::Ptr<AudioBuffer> m_buffer;

    // https://webaudio.github.io/web-audio-api/#dom-convolvernode-normalize
    bool m_normalize { true };
};

}
