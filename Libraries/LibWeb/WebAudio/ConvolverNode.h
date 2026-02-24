/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/ConvolverNodePrototype.h>
#include <LibWeb/WebAudio/AudioBuffer.h>
#include <LibWeb/WebAudio/AudioNode.h>

namespace Web::WebAudio {

// https://webaudio.github.io/web-audio-api/#ConvolverOptions
struct ConvolverOptions : AudioNodeOptions {
    GC::Ptr<AudioBuffer> buffer;
    bool disable_normalization { false };
};

// https://webaudio.github.io/web-audio-api/#ConvolverNode
class ConvolverNode : public AudioNode {
    WEB_PLATFORM_OBJECT(ConvolverNode, AudioNode);
    GC_DECLARE_ALLOCATOR(ConvolverNode);

public:
    virtual ~ConvolverNode() override;

    WebIDL::UnsignedLong number_of_inputs() override { return 1; }
    WebIDL::UnsignedLong number_of_outputs() override { return 1; }

    WebIDL::ExceptionOr<void> set_buffer(GC::Ptr<AudioBuffer>);
    GC::Ptr<AudioBuffer> buffer() const { return m_buffer; }

    void set_normalize(bool);
    bool normalize() const { return m_normalize; }

    WebIDL::ExceptionOr<void> set_channel_count(WebIDL::UnsignedLong) override;
    WebIDL::ExceptionOr<void> set_channel_count_mode(Bindings::ChannelCountMode) override;

    static WebIDL::ExceptionOr<GC::Ref<ConvolverNode>> create(JS::Realm&, GC::Ref<BaseAudioContext>, ConvolverOptions const& = {});
    static WebIDL::ExceptionOr<GC::Ref<ConvolverNode>> construct_impl(JS::Realm&, GC::Ref<BaseAudioContext>, ConvolverOptions const& = {});

protected:
    ConvolverNode(JS::Realm&, GC::Ref<BaseAudioContext>, ConvolverOptions const& = {});

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

private:
    bool impulse_buffer_is_valid(AudioBuffer const&) const;

    GC::Ptr<AudioBuffer> m_buffer;
    bool m_normalize { true };
};

}
