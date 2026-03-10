/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/WebAudio/MediaStreamAudioSourceNode.h>

#include <AK/QuickSort.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibWeb/DOM/IDLEventListener.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/WebAudio/AudioContext.h>
#include <LibWeb/WebIDL/CallbackType.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(MediaStreamAudioSourceNode);

MediaStreamAudioSourceNode::MediaStreamAudioSourceNode(JS::Realm& realm, GC::Ref<AudioContext> context, MediaStreamAudioSourceOptions const& options, GC::Ref<MediaCapture::MediaStreamTrack> track)
    : AudioNode(realm, context)
    , m_media_stream(*options.media_stream)
    , m_track(track)
    , m_provider_id(track->provider_id())
{
}

WebIDL::ExceptionOr<GC::Ref<MediaStreamAudioSourceNode>> MediaStreamAudioSourceNode::create(JS::Realm& realm, GC::Ref<AudioContext> context, MediaStreamAudioSourceOptions const& options)
{
    return construct_impl(realm, context, options);
}

WebIDL::ExceptionOr<GC::Ref<MediaStreamAudioSourceNode>> MediaStreamAudioSourceNode::construct_impl(JS::Realm& realm, GC::Ref<AudioContext> context, MediaStreamAudioSourceOptions const& options)
{
    if (!options.media_stream)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Missing mediaStream"_string };

    auto tracks = options.media_stream->get_audio_tracks();
    if (tracks.is_empty())
        return WebIDL::InvalidStateError::create(realm, "MediaStream has no audio tracks"_utf16);

    AK::quick_sort(tracks, [](auto const& a, auto const& b) {
        return a->id() < b->id();
    });

    auto track = tracks.first();
    return realm.create<MediaStreamAudioSourceNode>(realm, context, options, track);
}

void MediaStreamAudioSourceNode::initialize(JS::Realm& realm)
{
    Base::initialize(realm);

    auto self = GC::Ref<MediaStreamAudioSourceNode> { *this };
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

void MediaStreamAudioSourceNode::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_media_stream);
    visitor.visit(m_track);
}

}
