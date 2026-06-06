/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MediaElementAudioSourceNode.h>
#include <LibWeb/HTML/HTMLMediaElement.h>
#include <LibWeb/WebAudio/AudioContext.h>
#include <LibWeb/WebAudio/MediaElementAudioSourceNode.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(MediaElementAudioSourceNode);

MediaElementAudioSourceNode::MediaElementAudioSourceNode(GC::Ref<AudioContext> context, Bindings::MediaElementAudioSourceOptions const& options)
    : AudioNode(context)
    , m_media_element(*options.media_element)
{
}

MediaElementAudioSourceNode::~MediaElementAudioSourceNode() = default;

WebIDL::ExceptionOr<GC::Ref<MediaElementAudioSourceNode>> MediaElementAudioSourceNode::create(GC::Ref<AudioContext> context, Bindings::MediaElementAudioSourceOptions const& options)
{
    return GC::Heap::the().allocate<MediaElementAudioSourceNode>(context, options);
}

WebIDL::ExceptionOr<GC::Ref<MediaElementAudioSourceNode>> MediaElementAudioSourceNode::construct_impl(GC::Ref<AudioContext> context, Bindings::MediaElementAudioSourceOptions const& options)
{
    return create(context, options);
}

void MediaElementAudioSourceNode::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_media_element);
}

}
