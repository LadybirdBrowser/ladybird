/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/WebAudio/AudioNode.h>

namespace Web::WebAudio {

// https://webaudio.github.io/web-audio-api/#MediaElementAudioSourceOptions
struct MediaElementAudioSourceOptions {
    GC::Ptr<HTML::HTMLMediaElement> media_element;
};

// https://webaudio.github.io/web-audio-api/#MediaElementAudioSourceNode
class MediaElementAudioSourceNode final : public AudioNode {
    WEB_PLATFORM_OBJECT(MediaElementAudioSourceNode, AudioNode);
    GC_DECLARE_ALLOCATOR(MediaElementAudioSourceNode);

public:
    virtual ~MediaElementAudioSourceNode() override;

    static WebIDL::ExceptionOr<GC::Ref<MediaElementAudioSourceNode>> create(JS::Realm&, GC::Ref<AudioContext>, MediaElementAudioSourceOptions const&);
    static WebIDL::ExceptionOr<GC::Ref<MediaElementAudioSourceNode>> construct_impl(JS::Realm&, GC::Ref<AudioContext>, MediaElementAudioSourceOptions const&);

    virtual WebIDL::UnsignedLong number_of_inputs() override { return 0; }
    virtual WebIDL::UnsignedLong number_of_outputs() override { return 1; }

    GC::Ref<HTML::HTMLMediaElement> media_element() const { return m_media_element; }

private:
    MediaElementAudioSourceNode(JS::Realm&, GC::Ref<AudioContext>, MediaElementAudioSourceOptions const&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ref<HTML::HTMLMediaElement> m_media_element;
};

}
