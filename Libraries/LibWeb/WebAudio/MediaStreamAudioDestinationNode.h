/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/MediaCapture/MediaStream.h>
#include <LibWeb/WebAudio/AudioNode.h>

namespace Web::WebAudio {

// https://webaudio.github.io/web-audio-api/#MediaStreamAudioDestinationNode
class MediaStreamAudioDestinationNode final : public AudioNode {
    WEB_PLATFORM_OBJECT(MediaStreamAudioDestinationNode, AudioNode);
    GC_DECLARE_ALLOCATOR(MediaStreamAudioDestinationNode);

public:
    virtual ~MediaStreamAudioDestinationNode() override = default;

    static WebIDL::ExceptionOr<GC::Ref<MediaStreamAudioDestinationNode>> create(JS::Realm&, GC::Ref<AudioContext>, AudioNodeOptions const& = {});
    static WebIDL::ExceptionOr<GC::Ref<MediaStreamAudioDestinationNode>> construct_impl(JS::Realm&, GC::Ref<AudioContext>, AudioNodeOptions const& = {});

    virtual WebIDL::UnsignedLong number_of_inputs() override { return 1; }
    virtual WebIDL::UnsignedLong number_of_outputs() override { return 0; }

    GC::Ref<MediaCapture::MediaStream> stream() const { return m_stream; }

private:
    MediaStreamAudioDestinationNode(JS::Realm&, GC::Ref<AudioContext>);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ref<MediaCapture::MediaStream> m_stream;
};

}
