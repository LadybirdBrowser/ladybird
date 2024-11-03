/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/IDAllocator.h>
#include <AK/Time.h>
#include <LibGfx/Bitmap.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibMedia/PlaybackManager.h>
#include <LibMedia/Track.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/VideoTrackPrototype.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/HTMLMediaElement.h>
#include <LibWeb/HTML/HTMLVideoElement.h>
#include <LibWeb/HTML/VideoTrack.h>
#include <LibWeb/HTML/VideoTrackList.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(VideoTrack);

static IDAllocator s_video_track_id_allocator;

VideoTrack::VideoTrack(JS::Realm& realm, JS::NonnullGCPtr<HTMLMediaElement> media_element, NonnullOwnPtr<Media::PlaybackManager> playback_manager)
    : PlatformObject(realm)
    , m_media_element(media_element)
    , m_playback_manager(move(playback_manager))
{
    m_playback_manager->on_video_frame = [this](auto frame) {
        auto playback_position = static_cast<double>(position().to_milliseconds()) / 1000.0;

        if (is<HTMLVideoElement>(*m_media_element))
            verify_cast<HTMLVideoElement>(*m_media_element).set_current_frame({}, move(frame), playback_position);

        m_media_element->set_current_playback_position(playback_position);
    };

    m_playback_manager->on_playback_state_change = [this]() {
        switch (m_playback_manager->get_state()) {
        case Media::PlaybackState::Stopped: {
            auto playback_position_ms = static_cast<double>(duration().to_milliseconds());
            m_media_element->set_current_playback_position(playback_position_ms / 1000.0);
            break;
        }

        default:
            break;
        }
    };

    m_playback_manager->on_decoder_error = [this](auto error) {
        auto error_message = MUST(String::from_utf8(error.description()));
        m_media_element->set_decoder_error(move(error_message));
    };

    m_playback_manager->on_fatal_playback_error = [this](auto error) {
        auto error_message = MUST(String::from_utf8(error.string_literal()));
        m_media_element->set_decoder_error(move(error_message));
    };
}

VideoTrack::~VideoTrack()
{
    auto id = m_id.to_number<int>();
    VERIFY(id.has_value());

    s_video_track_id_allocator.deallocate(id.value());
}

void VideoTrack::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(VideoTrack);

    auto id = s_video_track_id_allocator.allocate();
    m_id = String::number(id);
}

void VideoTrack::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_media_element);
    visitor.visit(m_video_track_list);
}

void VideoTrack::play_video(Badge<HTMLVideoElement>)
{
    m_playback_manager->resume_playback();
}

void VideoTrack::pause_video(Badge<HTMLVideoElement>)
{
    m_playback_manager->pause_playback();
}

void VideoTrack::stop_video(Badge<HTMLVideoElement>)
{
    m_playback_manager->terminate_playback();
}

AK::Duration VideoTrack::position() const
{
    return m_playback_manager->current_playback_time();
}

AK::Duration VideoTrack::duration() const
{
    return m_playback_manager->selected_video_track().video_data().duration;
}

void VideoTrack::seek(AK::Duration position, MediaSeekMode seek_mode)
{
    switch (seek_mode) {
    case MediaSeekMode::Accurate:
        m_playback_manager->seek_to_timestamp(position, Media::PlaybackManager::SeekMode::Accurate);
        break;
    case MediaSeekMode::ApproximateForSpeed:
        m_playback_manager->seek_to_timestamp(position, Media::PlaybackManager::SeekMode::Fast);
        break;
    }
}

u64 VideoTrack::pixel_width() const
{
    return m_playback_manager->selected_video_track().video_data().pixel_width;
}

u64 VideoTrack::pixel_height() const
{
    return m_playback_manager->selected_video_track().video_data().pixel_height;
}

// https://html.spec.whatwg.org/multipage/media.html#dom-videotrack-selected
void VideoTrack::set_selected(bool selected)
{
    // On setting, it must select the track if the new value is true, and unselect it otherwise.
    if (m_selected == selected)
        return;

    // If the track is in a VideoTrackList, then all the other VideoTrack objects in that list must be unselected. (If the track is
    // no longer in a VideoTrackList object, then the track being selected or unselected has no effect beyond changing the value of
    // the attribute on the VideoTrack object.)
    if (m_video_track_list) {
        for (auto video_track : m_video_track_list->video_tracks()) {
            if (video_track.ptr() != this)
                video_track->m_selected = false;
        }

        // Whenever a track in a VideoTrackList that was previously not selected is selected, and whenever the selected track in a
        // VideoTrackList is unselected without a new track being selected in its stead, the user agent must queue a media element
        // task given the media element to fire an event named change at the VideoTrackList object. This task must be queued before
        // the task that fires the resize event, if any.
        auto previously_unselected_track_is_selected = !m_selected && selected;
        auto selected_track_was_unselected_without_another_selection = m_selected && !selected;

        if (previously_unselected_track_is_selected || selected_track_was_unselected_without_another_selection) {
            m_media_element->queue_a_media_element_task([this]() {
                m_video_track_list->dispatch_event(DOM::Event::create(realm(), HTML::EventNames::change));
            });
        }
    }

    m_selected = selected;

    // AD-HOC: Inform the video element node that we have (un)selected a video track for layout.
    if (is<HTMLVideoElement>(*m_media_element)) {
        auto& video_element = verify_cast<HTMLVideoElement>(*m_media_element);
        video_element.set_video_track(m_selected ? this : nullptr);
    }
}

}
