/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/VM.h>
#include <LibMedia/Sinks/AudioPullSink.h>
#include <LibWeb/Bindings/MediaElementAudioSourceNode.h>
#include <LibWeb/HTML/HTMLMediaElement.h>
#include <LibWeb/WebAudio/AudioContext.h>
#include <LibWeb/WebAudio/ControlMessageQueue.h>
#include <LibWeb/WebAudio/MediaElementAudioSourceNode.h>
#include <LibWeb/WebAudio/Rendering/RenderNodes.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(MediaElementAudioSourceNode);

MediaElementAudioSourceNode::MediaElementAudioSourceNode(GC::Ref<AudioContext> context, GC::Ref<HTML::HTMLMediaElement> media_element)
    : AudioNode(context)
    , m_media_element(media_element)
{
}

MediaElementAudioSourceNode::~MediaElementAudioSourceNode() = default;

WebIDL::ExceptionOr<GC::Ref<MediaElementAudioSourceNode>> MediaElementAudioSourceNode::create(GC::Ref<AudioContext> context, GC::Ref<HTML::HTMLMediaElement> media_element)
{
    // https://webaudio.github.io/web-audio-api/#MediaElementAudioSourceNode
    // If the sample rate of the HTMLMediaElement differs from the sample rate of the associated AudioContext, then
    // the output from the HTMLMediaElement must be resampled to match the context's sample rate.
    auto audio_pull_sink = TRY_OR_THROW_OOM(JS::VM::the(), Media::AudioPullSink::try_create(static_cast<u32>(context->sample_rate())));
    auto node = GC::Heap::the().allocate<MediaElementAudioSourceNode>(context, media_element);

    // https://webaudio.github.io/web-audio-api/#dom-mediaelementaudiosourcenode-mediaelementaudiosourcenode
    // 1. Initialize the AudioNode this, with context and options as arguments.
    AudioNodeDefaultOptions default_options {
        .channel_count = 2,
        .channel_count_mode = ChannelCountMode::Max,
        .channel_interpretation = ChannelInterpretation::Speakers,
    };
    TRY(node->initialize_audio_node_options(AudioNodeOptions {}, default_options));

    // https://webaudio.github.io/web-audio-api/#MediaElementAudioSourceNode
    // The HTMLMediaElement MUST behave in an identical fashion after the MediaElementAudioSourceNode has been
    // created, except that the rendered audio will no longer be heard directly, but instead will be heard as a
    // consequence of the MediaElementAudioSourceNode being connected through the routing graph. Thus pausing,
    // seeking, volume, src attribute changes, and other aspects of the HTMLMediaElement MUST behave as they normally
    // would if not used with a MediaElementAudioSourceNode.
    auto output_must_be_silenced = media_element->media_data_must_be_silenced();
    auto node_id = node->node_id();
    auto control_message_queue = context->control_message_queue();
    auto is_ticking = context->rendering_state() == AudioContextState::Running;
    if (media_element->attach_audio_pull_sink(audio_pull_sink, is_ticking, [node_id, control_message_queue = move(control_message_queue)](bool output_must_be_silenced) {
            control_message_queue->enqueue(NodeMessage { SetMediaElementSourceOutputSilenced { node_id, output_must_be_silenced } });
        })
        == HTML::HTMLMediaElement::AttachAudioPullSinkResult::AlreadyAttached) {
        // AD-HOC: The specification does not say what happens when an element already feeds another node. Chromium
        //         rejects the second node in the same way, while Firefox feeds every node from the element.
        return WebIDL::InvalidStateError::create("The HTMLMediaElement is already connected to a MediaElementAudioSourceNode"_utf16);
    }

    // NB: The element keeps feeding the node for as long as the context renders it, so the context roots the node like
    //     a playing scheduled source. Its render node never reports ended, so it stays rooted until the context is
    //     collected.
    context->add_playing_source(node);

    // NB: Initialize the render node with the current CORS state so it cannot render an unsilenced quantum before a
    //     subsequent control message is applied. Later changes are ordered after AddNode by the control queue.
    node->queue_render_node_creation(make<Rendering::MediaElementAudioSourceRenderNode>(node_id, BaseAudioContext::render_quantum_size(), move(audio_pull_sink), output_must_be_silenced));
    return node;
}

WebIDL::ExceptionOr<GC::Ref<MediaElementAudioSourceNode>> MediaElementAudioSourceNode::create_for_constructor(GC::Ref<AudioContext> context, GC::Ref<HTML::HTMLMediaElement> media_element)
{
    return create(context, media_element);
}

WebIDL::ExceptionOr<GC::Ref<MediaElementAudioSourceNode>> MediaElementAudioSourceNode::create_for_constructor(GC::Ref<AudioContext> context, Bindings::MediaElementAudioSourceOptions const& options)
{
    return create_for_constructor(context, *options.media_element);
}

void MediaElementAudioSourceNode::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_media_element);
}

}
