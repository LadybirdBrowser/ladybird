/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/WebAudio/MediaStreamTrackAudioSourceNode.h>

#include <LibJS/Runtime/NativeFunction.h>
#include <LibWeb/DOM/IDLEventListener.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/WebAudio/AudioContext.h>
#include <LibWeb/WebIDL/CallbackType.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(MediaStreamTrackAudioSourceNode);

MediaStreamTrackAudioSourceNode::MediaStreamTrackAudioSourceNode(JS::Realm& realm, GC::Ref<AudioContext> context, GC::Ref<MediaCapture::MediaStreamTrack> track)
    : AudioNode(realm, context)
    , m_track(track)
    , m_provider_id(track->provider_id())
{
}

WebIDL::ExceptionOr<GC::Ref<MediaStreamTrackAudioSourceNode>> MediaStreamTrackAudioSourceNode::create(JS::Realm& realm, GC::Ref<AudioContext> context, MediaStreamTrackAudioSourceOptions const& options)
{
    return construct_impl(realm, context, options);
}

WebIDL::ExceptionOr<GC::Ref<MediaStreamTrackAudioSourceNode>> MediaStreamTrackAudioSourceNode::construct_impl(JS::Realm& realm, GC::Ref<AudioContext> context, MediaStreamTrackAudioSourceOptions const& options)
{
    if (!options.media_stream_track)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Missing mediaStreamTrack"_string };

    if (!options.media_stream_track->is_audio())
        return WebIDL::InvalidStateError::create(realm, "MediaStreamTrack is not audio"_utf16);

    return realm.create<MediaStreamTrackAudioSourceNode>(realm, context, *options.media_stream_track);
}

void MediaStreamTrackAudioSourceNode::initialize(JS::Realm& realm)
{
    Base::initialize(realm);

    auto self = GC::Ref<MediaStreamTrackAudioSourceNode> { *this };
    auto ended_callback_function = JS::NativeFunction::create(
        realm,
        [self](JS::VM&) {
            self->context()->notify_audio_graph_changed();
            return JS::js_undefined();
        },
        0, Utf16FlyString {}, &realm);
    auto ended_callback = realm.heap().allocate<WebIDL::CallbackType>(*ended_callback_function, realm);
    m_track->add_event_listener_without_options(HTML::EventNames::ended, DOM::IDLEventListener::create(realm, ended_callback));
}

void MediaStreamTrackAudioSourceNode::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_track);
}

}
