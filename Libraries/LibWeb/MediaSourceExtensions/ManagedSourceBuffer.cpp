/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/ManagedSourceBuffer.h>
#include <LibWeb/MediaSourceExtensions/EventNames.h>
#include <LibWeb/MediaSourceExtensions/ManagedMediaSource.h>
#include <LibWeb/MediaSourceExtensions/ManagedSourceBuffer.h>

namespace Web::MediaSourceExtensions {

GC_DEFINE_ALLOCATOR(ManagedSourceBuffer);

ManagedSourceBuffer::ManagedSourceBuffer(ManagedMediaSource& media_source, GC::Ref<HTML::AudioTrackList> audio_tracks, GC::Ref<HTML::VideoTrackList> video_tracks, GC::Ref<HTML::TextTrackList> text_tracks)
    : SourceBuffer(media_source, audio_tracks, video_tracks, text_tracks)
{
}

ManagedSourceBuffer::~ManagedSourceBuffer() = default;

// https://w3c.github.io/media-source/#dom-managedsourcebuffer-onbufferedchange
void ManagedSourceBuffer::set_onbufferedchange(GC::Ptr<WebIDL::CallbackType> event_handler)
{
    set_event_handler_attribute(EventNames::bufferedchange, event_handler);
}

// https://w3c.github.io/media-source/#dom-managedsourcebuffer-onbufferedchange
GC::Ptr<WebIDL::CallbackType> ManagedSourceBuffer::onbufferedchange()
{
    return event_handler_attribute(EventNames::bufferedchange);
}

}
