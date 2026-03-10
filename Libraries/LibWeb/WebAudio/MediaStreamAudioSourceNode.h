/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/MediaCapture/MediaStream.h>
#include <LibWeb/WebAudio/AudioNode.h>

namespace Web::WebAudio {

// https://webaudio.github.io/web-audio-api/#MediaStreamAudioSourceOptions
struct MediaStreamAudioSourceOptions {
    GC::Ptr<MediaCapture::MediaStream> media_stream;
};

// https://webaudio.github.io/web-audio-api/#MediaStreamAudioSourceNode
class MediaStreamAudioSourceNode final : public AudioNode {
    WEB_PLATFORM_OBJECT(MediaStreamAudioSourceNode, AudioNode);
    GC_DECLARE_ALLOCATOR(MediaStreamAudioSourceNode);

public:
    virtual ~MediaStreamAudioSourceNode() override = default;

    static WebIDL::ExceptionOr<GC::Ref<MediaStreamAudioSourceNode>> create(JS::Realm&, GC::Ref<AudioContext>, MediaStreamAudioSourceOptions const&);
    static WebIDL::ExceptionOr<GC::Ref<MediaStreamAudioSourceNode>> construct_impl(JS::Realm&, GC::Ref<AudioContext>, MediaStreamAudioSourceOptions const&);

    virtual WebIDL::UnsignedLong number_of_inputs() override { return 0; }
    virtual WebIDL::UnsignedLong number_of_outputs() override { return 1; }

    GC::Ref<MediaCapture::MediaStream> media_stream() const { return m_media_stream; }
    GC::Ref<MediaCapture::MediaStreamTrack> track() const { return m_track; }

    u64 provider_id() const { return m_provider_id; }

private:
    MediaStreamAudioSourceNode(JS::Realm&, GC::Ref<AudioContext>, MediaStreamAudioSourceOptions const&, GC::Ref<MediaCapture::MediaStreamTrack>);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ref<MediaCapture::MediaStream> m_media_stream;
    GC::Ref<MediaCapture::MediaStreamTrack> m_track;
    u64 m_provider_id { 0 };
};

}
