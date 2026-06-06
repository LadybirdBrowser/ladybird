/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/WebAudio/AudioNode.h>

namespace Web::WebAudio {

// https://webaudio.github.io/web-audio-api/#MediaElementAudioSourceNode
class MediaElementAudioSourceNode final : public AudioNode {
    WEB_WRAPPABLE(MediaElementAudioSourceNode, AudioNode);
    GC_DECLARE_ALLOCATOR(MediaElementAudioSourceNode);

public:
    virtual ~MediaElementAudioSourceNode() override;

    static WebIDL::ExceptionOr<GC::Ref<MediaElementAudioSourceNode>> create(GC::Ref<AudioContext>, Bindings::MediaElementAudioSourceOptions const&);
    static WebIDL::ExceptionOr<GC::Ref<MediaElementAudioSourceNode>> construct_impl(GC::Ref<AudioContext>, Bindings::MediaElementAudioSourceOptions const&);

    virtual WebIDL::UnsignedLong number_of_inputs() override { return 0; }
    virtual WebIDL::UnsignedLong number_of_outputs() override { return 1; }

    GC::Ref<HTML::HTMLMediaElement> media_element() const { return m_media_element; }

private:
    MediaElementAudioSourceNode(GC::Ref<AudioContext>, Bindings::MediaElementAudioSourceOptions const&);
    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ref<HTML::HTMLMediaElement> m_media_element;
};

}
