/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2025, contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Time.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SourceBufferPrototype.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/HTMLMediaElement.h>
#include <LibWeb/HTML/TimeRanges.h>
#include <LibWeb/MediaSourceExtensions/EventNames.h>
#include <LibWeb/MediaSourceExtensions/MediaSource.h>
#include <LibWeb/MediaSourceExtensions/SourceBuffer.h>
#include <LibWeb/MimeSniff/MimeType.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/Buffers.h>
#include <LibMedia/FFmpeg/MSEDemuxer.h>
#include <LibMedia/PlaybackManager.h>
#include <LibMedia/Sinks/DisplayingVideoSink.h>

namespace Web::MediaSourceExtensions {

GC_DEFINE_ALLOCATOR(SourceBuffer);

SourceBuffer::SourceBuffer(JS::Realm& realm)
    : DOM::EventTarget(realm)
{
}

SourceBuffer::SourceBuffer(JS::Realm& realm, MediaSource& media_source, MimeSniff::MimeType const&)
    : DOM::EventTarget(realm)
    , m_media_source(&media_source)
{
}

SourceBuffer::~SourceBuffer() = default;

void SourceBuffer::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SourceBuffer);
    Base::initialize(realm);

    // Initialize empty TimeRanges for buffered property
    m_buffered = realm.create<HTML::TimeRanges>(realm);
}

void SourceBuffer::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_media_source);
    visitor.visit(m_buffered);
}

// https://w3c.github.io/media-source/#dom-sourcebuffer-onupdatestart
void SourceBuffer::set_onupdatestart(GC::Ptr<WebIDL::CallbackType> event_handler)
{
    set_event_handler_attribute(EventNames::updatestart, event_handler);
}

// https://w3c.github.io/media-source/#dom-sourcebuffer-onupdatestart
GC::Ptr<WebIDL::CallbackType> SourceBuffer::onupdatestart()
{
    return event_handler_attribute(EventNames::updatestart);
}

// https://w3c.github.io/media-source/#dom-sourcebuffer-onupdate
void SourceBuffer::set_onupdate(GC::Ptr<WebIDL::CallbackType> event_handler)
{
    set_event_handler_attribute(EventNames::update, event_handler);
}

// https://w3c.github.io/media-source/#dom-sourcebuffer-onupdate
GC::Ptr<WebIDL::CallbackType> SourceBuffer::onupdate()
{
    return event_handler_attribute(EventNames::update);
}

// https://w3c.github.io/media-source/#dom-sourcebuffer-onupdateend
void SourceBuffer::set_onupdateend(GC::Ptr<WebIDL::CallbackType> event_handler)
{
    set_event_handler_attribute(EventNames::updateend, event_handler);
}

// https://w3c.github.io/media-source/#dom-sourcebuffer-onupdateend
GC::Ptr<WebIDL::CallbackType> SourceBuffer::onupdateend()
{
    return event_handler_attribute(EventNames::updateend);
}

// https://w3c.github.io/media-source/#dom-sourcebuffer-onerror
void SourceBuffer::set_onerror(GC::Ptr<WebIDL::CallbackType> event_handler)
{
    set_event_handler_attribute(EventNames::error, event_handler);
}

// https://w3c.github.io/media-source/#dom-sourcebuffer-onerror
GC::Ptr<WebIDL::CallbackType> SourceBuffer::onerror()
{
    return event_handler_attribute(EventNames::error);
}

// https://w3c.github.io/media-source/#dom-sourcebuffer-onabort
void SourceBuffer::set_onabort(GC::Ptr<WebIDL::CallbackType> event_handler)
{
    set_event_handler_attribute(EventNames::abort, event_handler);
}

// https://w3c.github.io/media-source/#dom-sourcebuffer-onabort
GC::Ptr<WebIDL::CallbackType> SourceBuffer::onabort()
{
    return event_handler_attribute(EventNames::abort);
}

// https://w3c.github.io/media-source/#dom-sourcebuffer-buffered
GC::Ref<HTML::TimeRanges> SourceBuffer::buffered() const
{
    // Return the buffered time ranges
    return *m_buffered;
}

void SourceBuffer::set_timestamp_offset(double offset)
{
    if (m_timestamp_offset == offset)
        return;

    dbgln("MSE: SourceBuffer::set_timestamp_offset() called: old={}, new={}", m_timestamp_offset, offset);
    m_timestamp_offset = offset;

    if (!m_demuxer)
        return;

    m_demuxer->set_timestamp_offset(AK::Duration::from_seconds_f64(offset));
    refresh_buffered_ranges();

    if (m_media_source)
        m_media_source->source_buffer_data_appended();
}

// https://w3c.github.io/media-source/#dom-sourcebuffer-appendbuffer
WebIDL::ExceptionOr<void> SourceBuffer::append_buffer(GC::Root<WebIDL::BufferSource> const& data)
{
    // 1. If data is null then throw a TypeError exception and abort these steps.
    if (!data.ptr())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Data cannot be null"_string };

    // 2. If this object has been removed from the sourceBuffers attribute of the parent media source then throw an InvalidStateError exception and abort these steps.
    if (!m_media_source)
        return WebIDL::InvalidStateError::create(realm(), "SourceBuffer has been removed from MediaSource"_utf16);

    // 3. If the updating attribute equals true, then throw an InvalidStateError exception and abort these steps.
    if (m_updating)
        return WebIDL::InvalidStateError::create(realm(), "SourceBuffer is currently updating"_utf16);

    // 4. If the readyState attribute of the parent media source is not in the "open" state then throw an InvalidStateError exception and abort these steps.
    if (m_media_source->ready_state() != Bindings::ReadyState::Open)
        return WebIDL::InvalidStateError::create(realm(), "MediaSource readyState is not 'open'"_utf16);

    // 5. If the HTMLMediaElement.error attribute is not null, then throw an InvalidStateError exception and abort these steps.
    // FIXME: Check media element error state

    // 6. Extract the byte buffer from data
    auto buffer_result = WebIDL::get_buffer_source_copy(*data->raw_object());
    if (buffer_result.is_error())
        return WebIDL::OperationError::create(realm(), "Failed to copy buffer data"_utf16);

    auto buffer = buffer_result.release_value();

    dbgln("MSE: append_buffer() called with {} bytes (pending buffers: {})",
          buffer.size(), m_pending_buffers.size());

    // 7. Add data to the end of the input buffer
    MUST(m_pending_buffers.try_append(move(buffer)));

    // 8. Set the updating attribute to true
    m_updating = true;

    // 9. Queue a task to fire an event named updatestart at this SourceBuffer object
    dispatch_event(DOM::Event::create(realm(), EventNames::updatestart));

    // 10. Asynchronously run the buffer append algorithm
    schedule_update_end();

    return {};
}

// https://w3c.github.io/media-source/#dom-sourcebuffer-abort
WebIDL::ExceptionOr<void> SourceBuffer::abort()
{
    // 1. If this object has been removed from the sourceBuffers attribute of the parent media source then throw an InvalidStateError exception and abort these steps.
    if (!m_media_source)
        return WebIDL::InvalidStateError::create(realm(), "SourceBuffer has been removed from MediaSource"_utf16);

    // 2. If the readyState attribute of the parent media source is not in the "open" state then throw an InvalidStateError exception and abort these steps.
    if (m_media_source->ready_state() != Bindings::ReadyState::Open)
        return WebIDL::InvalidStateError::create(realm(), "MediaSource readyState is not 'open'"_utf16);

    // 3. If the updating attribute equals true, then run the following steps:
    if (m_updating) {
        // 3.1. Abort the buffer append algorithm if it is running
        m_pending_buffers.clear();

        // 3.2. Set the updating attribute to false
        m_updating = false;

        // 3.3. Queue a task to fire an event named abort at this SourceBuffer object
        dispatch_event(DOM::Event::create(realm(), EventNames::abort));

        // 3.4. Queue a task to fire an event named updateend at this SourceBuffer object
        dispatch_event(DOM::Event::create(realm(), EventNames::updateend));
    }

    // FIXME: 4. Run the reset parser state algorithm

    // FIXME: 5. Set appendWindowStart to the presentation start time
    // FIXME: 6. Set appendWindowEnd to positive Infinity

    return {};
}

// https://w3c.github.io/media-source/#dom-sourcebuffer-remove
WebIDL::ExceptionOr<void> SourceBuffer::remove(double start, double end)
{
    // 1. If this object has been removed from the sourceBuffers attribute of the parent media source then throw an InvalidStateError exception and abort these steps.
    if (!m_media_source)
        return WebIDL::InvalidStateError::create(realm(), "SourceBuffer has been removed from MediaSource"_utf16);

    // 2. If the updating attribute equals true, then throw an InvalidStateError exception and abort these steps.
    if (m_updating)
        return WebIDL::InvalidStateError::create(realm(), "SourceBuffer is currently updating"_utf16);

    // 3. If duration equals NaN, then throw a TypeError exception and abort these steps.
    if (isnan(m_media_source->duration()))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "MediaSource duration is NaN"_string };

    // 4. If start is negative or greater than duration, then throw a TypeError exception and abort these steps.
    if (start < 0 || start > m_media_source->duration())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Invalid start value"_string };

    // 5. If end is less than or equal to start or end equals NaN, then throw a TypeError exception and abort these steps.
    if (end <= start || isnan(end))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Invalid end value"_string };

    // 6. If the readyState attribute of the parent media source is not in the "open" state then throw an InvalidStateError exception and abort these steps.
    if (m_media_source->ready_state() != Bindings::ReadyState::Open)
        return WebIDL::InvalidStateError::create(realm(), "MediaSource readyState is not 'open'"_utf16);

    // 7. Set the updating attribute to true
    m_updating = true;

    // 8. Queue a task to fire an event named updatestart at this SourceBuffer object
    dispatch_event(DOM::Event::create(realm(), EventNames::updatestart));

    // 9. Return and run the coded frame removal algorithm asynchronously
    HTML::queue_a_microtask(nullptr, GC::create_function(heap(), [this] {
        // FIXME: Actually remove coded frames from track buffers in the given range
        // For now, just complete successfully

        m_updating = false;
        dispatch_event(DOM::Event::create(realm(), EventNames::update));
        dispatch_event(DOM::Event::create(realm(), EventNames::updateend));
    }));

    return {};
}

void SourceBuffer::refresh_buffered_ranges()
{
    if (!m_demuxer || !m_buffered)
        return;

    auto buffered_start = m_demuxer->buffered_start_time();
    auto buffered_end = m_demuxer->buffered_end_time();

    if (buffered_end < buffered_start)
        buffered_end = buffered_start;

    m_buffered->clear();
    m_buffered->add_range(buffered_start.to_seconds(), buffered_end.to_seconds());

    // Log buffered ranges for debugging (only occasionally)
    static size_t log_counter = 0;
    if (log_counter++ % 50 == 0) {
        dbgln("MSE: SourceBuffer buffered ranges updated: {}s - {}s (duration: {}s)",
            buffered_start.to_seconds(), buffered_end.to_seconds(),
            (buffered_end - buffered_start).to_seconds());
    }
}

void SourceBuffer::schedule_update_end()
{
    // Queue a microtask to process the buffer asynchronously
    // For now, we'll just immediately fire the events since we're not actually parsing the data yet
    HTML::queue_a_microtask(nullptr, GC::create_function(heap(), [this] {
        process_append_buffer();
    }));
}

void SourceBuffer::process_append_buffer()
{
    // Get the buffered data to process
    if (m_pending_buffers.is_empty()) {
        // No data to process, just fire events
        dbgln("MSE: process_append_buffer() - no pending buffers");
        m_updating = false;
        dispatch_event(DOM::Event::create(realm(), EventNames::update));
        dispatch_event(DOM::Event::create(realm(), EventNames::updateend));
        return;
    }

    auto buffer_data = m_pending_buffers.take_first();
    dbgln("MSE: process_append_buffer() - processing buffer of {} bytes ({} remaining)",
          buffer_data.size(), m_pending_buffers.size());

    // First append: Initialize demuxer with initialization segment only
    if (!m_demuxer) {
        // Create MSEDemuxer
        auto demuxer_result = Media::FFmpeg::MSEDemuxer::create();
        if (demuxer_result.is_error()) {
            dbgln("Failed to create MSEDemuxer: {}", demuxer_result.error());
            m_updating = false;
            dispatch_event(DOM::Event::create(realm(), EventNames::error));
            dispatch_event(DOM::Event::create(realm(), EventNames::updateend));
            return;
        }
        m_demuxer = demuxer_result.release_value();
        m_demuxer->set_timestamp_offset(AK::Duration::from_seconds_f64(m_timestamp_offset));

        // Append initialization segment (contains codec info but no frames)
        auto init_result = m_demuxer->append_initialization_segment(buffer_data);
        if (init_result.is_error()) {
            dbgln("Failed to append initialization segment: {}", init_result.error());
            m_updating = false;
            dispatch_event(DOM::Event::create(realm(), EventNames::error));
            dispatch_event(DOM::Event::create(realm(), EventNames::updateend));
            return;
        }

        dbgln("MSE: Initialization segment appended. Waiting for first media segment before creating PlaybackManager.");
    } else {
        // Subsequent appends: Append media segments
        auto append_result = m_demuxer->append_media_segment(buffer_data);
        if (append_result.is_error()) {
            dbgln("Failed to append media segment: {}", append_result.error());
            m_updating = false;
            dispatch_event(DOM::Event::create(realm(), EventNames::error));
            dispatch_event(DOM::Event::create(realm(), EventNames::updateend));
            return;
        }

        // After appending the FIRST media segment, create or reuse PlaybackManager
        // This ensures FFmpeg has actual frame data before trying to read
        if (!m_playback_manager) {
            bool created_new_playback_manager = false;

            // Check if HTMLMediaElement already has a PlaybackManager from another SourceBuffer
            if (m_media_source && m_media_source->media_element() && m_media_source->media_element()->mse_playback_manager()) {
                dbgln("MSE: Reusing existing PlaybackManager from another SourceBuffer");
                m_playback_manager = m_media_source->media_element()->mse_playback_manager();
                created_new_playback_manager = false;
            } else {
                dbgln("MSE: First media segment appended. Creating PlaybackManager now that we have frame data.");

                auto playback_manager_result = Media::PlaybackManager::try_create_for_mse(NonnullRefPtr { *m_demuxer });
                if (playback_manager_result.is_error()) {
                    dbgln("Failed to create PlaybackManager: {}", playback_manager_result.error());
                    m_updating = false;
                    dispatch_event(DOM::Event::create(realm(), EventNames::error));
                    dispatch_event(DOM::Event::create(realm(), EventNames::updateend));
                    return;
                }
                m_playback_manager = playback_manager_result.release_value();
                created_new_playback_manager = true;
            }

            // Connect to HTMLMediaElement
            if (m_media_source && m_media_source->media_element()) {
                auto& media_element = *m_media_source->media_element();

                // Pass PlaybackManager to HTMLMediaElement so it can control playback
                // (This is safe even if reusing existing - it's a no-op if already set)
                media_element.set_mse_playback_manager(m_playback_manager);

                // Get tracks from this SourceBuffer's demuxer
                auto video_tracks = m_demuxer->get_tracks_for_type(Media::TrackType::Video);
                auto audio_tracks = m_demuxer->get_tracks_for_type(Media::TrackType::Audio);

                // Handle video tracks - add them dynamically if reusing PlaybackManager
                if (!video_tracks.is_error() && !video_tracks.value().is_empty()) {
                    for (auto const& video_track : video_tracks.value()) {
                        // Add video track to PlaybackManager from this SourceBuffer's demuxer
                        // This handles the case where PlaybackManager was created by a different SourceBuffer
                        auto add_result = m_playback_manager->add_video_track_from_demuxer(NonnullRefPtr { *m_demuxer }, video_track);
                        if (add_result.is_error()) {
                            dbgln("MSE: Failed to add video track: {}", add_result.error());
                            continue;
                        }

                        // Get or create DisplayingVideoSink
                        auto video_sink = m_playback_manager->get_or_create_the_displaying_video_sink_for_track(video_track);

                        // Store reference in HTMLMediaElement
                        media_element.set_mse_video_sink(video_sink);
                        dbgln("MSE: Connected video sink to HTMLMediaElement");

                        // Add the video track to HTMLMediaElement's video_tracks() list
                        media_element.add_mse_video_track(video_track);
                    }
                }

                // Handle audio tracks - add them dynamically if reusing PlaybackManager
                if (!audio_tracks.is_error() && !audio_tracks.value().is_empty()) {
                    for (auto const& audio_track : audio_tracks.value()) {
                        // Add audio track to PlaybackManager from this SourceBuffer's demuxer
                        // This handles the case where PlaybackManager was created by a different SourceBuffer
                        auto add_result = m_playback_manager->add_audio_track_from_demuxer(NonnullRefPtr { *m_demuxer }, audio_track);
                        if (add_result.is_error()) {
                            dbgln("MSE: Failed to add audio track: {}", add_result.error());
                            continue;
                        }

                        // Enable the audio track
                        m_playback_manager->enable_an_audio_track(audio_track);
                        dbgln("MSE: Enabled audio track");

                        // Add the audio track to HTMLMediaElement's audio_tracks() list
                        media_element.add_mse_audio_track(audio_track);
                    }
                }

                // Only do pause/seek/play if we CREATED a new PlaybackManager
                // Don't interfere with playback if we're reusing an existing one
                if (created_new_playback_manager) {
                    // HLS streams often don't start at timestamp 0
                    // Pause playback, seek to first available keyframe, then resume
                    dbgln("MSE: Pausing to seek to first available frame");
                    m_playback_manager->pause();

                    // Seek forward from 0 to find the first keyframe
                    dbgln("MSE: Seeking forward from timestamp 0 to find first keyframe");
                    m_playback_manager->seek(AK::Duration::from_milliseconds(1), Media::SeekMode::FastAfter);

                    // Resume playback after seek
                    m_playback_manager->play();
                    dbgln("MSE: Resumed playback after seeking");
                }
            }

            m_first_media_segment_appended = true;
        }
    }

    refresh_buffered_ranges();

    // Success - fire events
    m_updating = false;
    dispatch_event(DOM::Event::create(realm(), EventNames::update));

    // Notify MediaSource that data was appended
    if (m_media_source) {
        m_media_source->source_buffer_data_appended();
    }

    dispatch_event(DOM::Event::create(realm(), EventNames::updateend));
}

}
