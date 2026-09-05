/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Forward.h>
#include <LibWeb/WebAudio/AudioNode.h>

namespace Web::Bindings {

struct MediaElementAudioSourceOptions;

}

namespace Web::WebAudio {

// https://webaudio.github.io/web-audio-api/#MediaElementAudioSourceNode
class MediaElementAudioSourceNode final : public AudioNode {
    WEB_WRAPPABLE(MediaElementAudioSourceNode, AudioNode);
    GC_DECLARE_ALLOCATOR(MediaElementAudioSourceNode);

public:
    static constexpr size_t media_element_offset() { return offsetof(MediaElementAudioSourceNode, m_media_element); }
    virtual ~MediaElementAudioSourceNode() override;

    static WebIDL::ExceptionOr<GC::Ref<MediaElementAudioSourceNode>> create(GC::Ref<AudioContext>, GC::Ref<HTML::HTMLMediaElement>);
    static WebIDL::ExceptionOr<GC::Ref<MediaElementAudioSourceNode>> create_for_constructor(GC::Ref<AudioContext>, GC::Ref<HTML::HTMLMediaElement>);
    static WebIDL::ExceptionOr<GC::Ref<MediaElementAudioSourceNode>> create_for_constructor(GC::Ref<AudioContext>, Bindings::MediaElementAudioSourceOptions const&);

    virtual WebIDL::UnsignedLong number_of_inputs() override { return 0; }
    virtual WebIDL::UnsignedLong number_of_outputs() override { return 1; }

    GC::Ref<HTML::HTMLMediaElement> media_element() const { return m_media_element; }

private:
    MediaElementAudioSourceNode(GC::Ref<AudioContext>, GC::Ref<HTML::HTMLMediaElement>);
    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ref<HTML::HTMLMediaElement> m_media_element;
};

}
