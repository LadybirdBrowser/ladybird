/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefPtr.h>
#include <LibWeb/Bindings/MediaStreamAudioSourceNode.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebAudio/AudioNode.h>

namespace Media {

class SpscAudioFrameRing;

}

namespace Web::MediaCapture {

struct AudioFrameSink;

}

namespace Web::WebAudio {

using MediaStreamAudioSourceOptions = Bindings::MediaStreamAudioSourceOptions;

// https://webaudio.github.io/web-audio-api/#MediaStreamAudioSourceNode
class MediaStreamAudioSourceNode final : public AudioNode {
    WEB_WRAPPABLE(MediaStreamAudioSourceNode, AudioNode);
    GC_DECLARE_ALLOCATOR(MediaStreamAudioSourceNode);

public:
    static constexpr bool OVERRIDES_FINALIZE = true;

    virtual ~MediaStreamAudioSourceNode() override;

    static WebIDL::ExceptionOr<GC::Ref<MediaStreamAudioSourceNode>> create(GC::Ref<AudioContext>, MediaStreamAudioSourceOptions const&);
    static WebIDL::ExceptionOr<GC::Ref<MediaStreamAudioSourceNode>> create_for_constructor(GC::Ref<AudioContext>, MediaStreamAudioSourceOptions const&);

    virtual WebIDL::UnsignedLong number_of_inputs() override { return 0; }
    virtual WebIDL::UnsignedLong number_of_outputs() override { return 1; }

    // https://webaudio.github.io/web-audio-api/#dom-mediastreamaudiosourcenode-mediastream
    GC::Ref<MediaCapture::MediaStream> media_stream() const { return m_media_stream; }

private:
    MediaStreamAudioSourceNode(GC::Ref<AudioContext>, MediaStreamAudioSourceOptions const&);

    virtual void visit_edges(Cell::Visitor&) override;
    virtual void finalize() override;

    void attach_input_track();

    // https://webaudio.github.io/web-audio-api/#dom-mediastreamaudiosourcenode-mediastream
    GC::Ref<MediaCapture::MediaStream> m_media_stream;

    // [[input track]]: the first audio track of the input stream, ordered by track id.
    GC::Ptr<MediaCapture::MediaStreamTrack> m_input_track;

    // Carries the track's audio from the producing thread to the render thread; the sink is
    // registered on the input track and only ever references the ring, never this GC node.
    RefPtr<Media::SpscAudioFrameRing> m_ring;
    RefPtr<MediaCapture::AudioFrameSink> m_sink;
};

}
