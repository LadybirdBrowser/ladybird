/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MediaElementAudioSourceNodePrototype.h>
#include <LibWeb/HTML/HTMLMediaElement.h>
#include <LibWeb/WebAudio/AudioContext.h>
#include <LibWeb/WebAudio/MediaElementAudioSourceNode.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(MediaElementAudioSourceNode);

// https://webaudio.github.io/web-audio-api/#mediaelementaudiosourcenode
MediaElementAudioSourceNode::MediaElementAudioSourceNode(JS::Realm& realm, GC::Ref<AudioContext> context, MediaElementAudioSourceOptions const& options)
    : AudioNode(realm, context)
    , m_media_element(*options.media_element)
    , m_provider(MediaElementAudioSourceProvider::create(8, 131072))
{
    // A MediaElementAudioSourceNode represents an audio source from an HTMLMediaElement.
    // The media element may have an arbitrary number of channels.
    m_provider->set_target_sample_rate(static_cast<u32>(context->sample_rate()));
    m_media_element->set_webaudio_audio_tap(m_provider);
}

MediaElementAudioSourceNode::~MediaElementAudioSourceNode()
{
}

void MediaElementAudioSourceNode::finalize()
{
    Base::finalize();

    // NOTE: finalize() is called by the GC in a separate pass before destruction.
    // We avoid doing this work in the destructor since GC sweep order is not guaranteed,
    // and the associated HTMLMediaElement may already be poisoned when our destructor runs.
    if (m_media_element->state() == GC::Cell::State::Live)
        m_media_element->clear_webaudio_audio_tap();
}

WebIDL::ExceptionOr<GC::Ref<MediaElementAudioSourceNode>> MediaElementAudioSourceNode::create(JS::Realm& realm, GC::Ref<AudioContext> context, MediaElementAudioSourceOptions const& options)
{
    return construct_impl(realm, context, options);
}

WebIDL::ExceptionOr<GC::Ref<MediaElementAudioSourceNode>> MediaElementAudioSourceNode::construct_impl(JS::Realm& realm, GC::Ref<AudioContext> context, MediaElementAudioSourceOptions const& options)
{
    return realm.create<MediaElementAudioSourceNode>(realm, context, options);
}

void MediaElementAudioSourceNode::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(MediaElementAudioSourceNode);
    Base::initialize(realm);
}

void MediaElementAudioSourceNode::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_media_element);
}

}
