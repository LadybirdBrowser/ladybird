/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/StereoPannerNodePrototype.h>
#include <LibWeb/WebAudio/AudioNode.h>

namespace Web::WebAudio {

// https://webaudio.github.io/web-audio-api/#StereoPannerOptions
struct StereoPannerOptions : AudioNodeOptions {
    float pan { 0 };
};

// https://webaudio.github.io/web-audio-api/#stereopannernode
class StereoPannerNode : public AudioNode {
    WEB_PLATFORM_OBJECT(StereoPannerNode, AudioNode);
    GC_DECLARE_ALLOCATOR(StereoPannerNode);

public:
    virtual ~StereoPannerNode() override;

    static WebIDL::ExceptionOr<GC::Ref<StereoPannerNode>> create(JS::Realm&, GC::Ref<BaseAudioContext>, StereoPannerOptions const& = {});
    static WebIDL::ExceptionOr<GC::Ref<StereoPannerNode>> construct_impl(JS::Realm&, GC::Ref<BaseAudioContext>, StereoPannerOptions const& = {});

    WebIDL::UnsignedLong number_of_inputs() override { return 1; }
    WebIDL::UnsignedLong number_of_outputs() override { return 1; }

    WebIDL::ExceptionOr<void> set_channel_count_mode(Bindings::ChannelCountMode) override;
    WebIDL::ExceptionOr<void> set_channel_count(WebIDL::UnsignedLong) override;

    GC::Ref<AudioParam const> pan() const { return m_pan; }

protected:
    StereoPannerNode(JS::Realm&, GC::Ref<BaseAudioContext>, StereoPannerOptions const& = {});

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

private:
    // https://webaudio.github.io/web-audio-api/#dom-stereopannernode-pan
    GC::Ref<AudioParam> m_pan;
};

}
