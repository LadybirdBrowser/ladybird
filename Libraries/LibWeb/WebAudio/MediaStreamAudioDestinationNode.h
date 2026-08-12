/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefPtr.h>
#include <LibWeb/Bindings/MediaStreamAudioDestinationNode.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebAudio/AudioNode.h>
#include <LibWeb/WebAudio/Rendering/MediaStreamDestinationRenderNode.h>

namespace Web::WebAudio {

// https://webaudio.github.io/web-audio-api/#MediaStreamAudioDestinationNode
class MediaStreamAudioDestinationNode final : public AudioNode {
    WEB_WRAPPABLE(MediaStreamAudioDestinationNode, AudioNode);
    GC_DECLARE_ALLOCATOR(MediaStreamAudioDestinationNode);

public:
    static constexpr bool OVERRIDES_FINALIZE = true;

    virtual ~MediaStreamAudioDestinationNode() override;

    static WebIDL::ExceptionOr<GC::Ref<MediaStreamAudioDestinationNode>> create(GC::Ref<AudioContext>, AudioNodeOptions const& = {});
    static WebIDL::ExceptionOr<GC::Ref<MediaStreamAudioDestinationNode>> create_for_constructor(GC::Ref<AudioContext>, AudioNodeOptions const& = {});

    virtual WebIDL::UnsignedLong number_of_inputs() override { return 1; }
    virtual WebIDL::UnsignedLong number_of_outputs() override { return 0; }

    // https://webaudio.github.io/web-audio-api/#dom-mediastreamaudiodestinationnode-stream
    GC::Ref<MediaCapture::MediaStream> stream() const { return m_stream; }

private:
    MediaStreamAudioDestinationNode(GC::Ref<AudioContext>, GC::Ref<MediaCapture::MediaStream>, GC::Ref<MediaCapture::MediaStreamTrack>);

    virtual void visit_edges(Cell::Visitor&) override;
    virtual void finalize() override;

    // https://webaudio.github.io/web-audio-api/#dom-mediastreamaudiodestinationnode-stream
    GC::Ref<MediaCapture::MediaStream> m_stream;

    GC::Ref<MediaCapture::MediaStreamTrack> m_track;

    RefPtr<Rendering::MediaStreamDestinationSharedState> m_shared_state;
};

}
