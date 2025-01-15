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

MediaElementAudioSourceNode::MediaElementAudioSourceNode(JS::Realm& realm, GC::Ref<AudioContext> context, MediaElementAudioSourceOptions const& options)
    : AudioNode(realm, context)
    , m_media_element(*options.media_element)
{
}

MediaElementAudioSourceNode::~MediaElementAudioSourceNode() = default;

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
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(MediaElementAudioSourceNode);
}

void MediaElementAudioSourceNode::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_media_element);
}

}
