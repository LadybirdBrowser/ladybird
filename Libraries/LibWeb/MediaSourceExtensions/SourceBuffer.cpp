/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibMedia/PlaybackManager.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SourceBufferPrototype.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/AudioTrackList.h>
#include <LibWeb/HTML/HTMLMediaElement.h>
#include <LibWeb/HTML/TextTrackList.h>
#include <LibWeb/HTML/TimeRanges.h>
#include <LibWeb/HTML/VideoTrackList.h>
#include <LibWeb/MediaSourceExtensions/EventNames.h>
#include <LibWeb/MediaSourceExtensions/MediaSource.h>
#include <LibWeb/MediaSourceExtensions/SourceBuffer.h>
#include <LibWeb/MediaSourceExtensions/SourceBufferList.h>
#include <LibWeb/MediaSourceExtensions/SourceBufferProcessor.h>
#include <LibWeb/MediaSourceExtensions/TrackBuffer.h>
#include <LibWeb/MediaSourceExtensions/TrackBufferDemuxer.h>
#include <LibWeb/MediaSourceExtensions/WebMByteStreamParser.h>
#include <LibWeb/MimeSniff/MimeType.h>
#include <LibWeb/WebIDL/Buffers.h>
#include <LibWeb/WebIDL/QuotaExceededError.h>

namespace Web::MediaSourceExtensions {

GC_DEFINE_ALLOCATOR(SourceBuffer);

SourceBuffer::SourceBuffer(JS::Realm& realm, MediaSource& media_source)
    : DOM::EventTarget(realm)
    , m_media_source(media_source)
    , m_processor(adopt_ref(*new SourceBufferProcessor()))
    , m_audio_tracks(realm.create<HTML::AudioTrackList>(realm))
    , m_video_tracks(realm.create<HTML::VideoTrackList>(realm))
    , m_text_tracks(realm.create<HTML::TextTrackList>(realm))
{
    m_processor->set_duration_change_callback([self = GC::Weak(*this)](double new_duration) {
        if (!self)
            return;
        // https://w3c.github.io/media-source/#sourcebuffer-init-segment-received
        // 1. Update the duration attribute if it currently equals NaN:
        if (isnan(self->m_media_source->duration()))
            self->m_media_source->run_duration_change_algorithm(new_duration);
    });

    m_processor->set_first_initialization_segment_callback([self = GC::Weak(*this)](InitializationSegmentData&& init_data) {
        if (!self)
            return;
        self->on_first_initialization_segment_processed(init_data);
    });

    m_processor->set_append_error_callback([self = GC::Weak(*this)]() {
        if (!self)
            return;
        self->run_append_error_algorithm();
    });

    m_processor->set_coded_frame_processing_done_callback([self = GC::Weak(*this)]() {
        if (!self)
            return;
        self->update_ready_state_and_duration_after_coded_frame_processing();
    });

    m_processor->set_append_done_callback([self = GC::Weak(*this)]() {
        if (!self)
            return;
        self->finish_buffer_append();
    });
}

SourceBuffer::~SourceBuffer() = default;

static Bindings::AppendMode processor_mode_to_bindings(AppendMode mode)
{
    switch (mode) {
    case AppendMode::Segments:
        return Bindings::AppendMode::Segments;
    case AppendMode::Sequence:
        return Bindings::AppendMode::Sequence;
    }
    VERIFY_NOT_REACHED();
}

static AppendMode bindings_mode_to_processor(Bindings::AppendMode mode)
{
    switch (mode) {
    case Bindings::AppendMode::Segments:
        return AppendMode::Segments;
    case Bindings::AppendMode::Sequence:
        return AppendMode::Sequence;
    }
    VERIFY_NOT_REACHED();
}

static Bindings::TextTrackKind media_track_kind_to_text_track_kind(Media::Track::Kind kind)
{
    switch (kind) {
    case Media::Track::Kind::Captions:
        return Bindings::TextTrackKind::Captions;
    case Media::Track::Kind::Descriptions:
        return Bindings::TextTrackKind::Descriptions;
    case Media::Track::Kind::Metadata:
        return Bindings::TextTrackKind::Metadata;
    case Media::Track::Kind::Subtitles:
        return Bindings::TextTrackKind::Subtitles;
    default:
        return Bindings::TextTrackKind::Metadata;
    }
}

void SourceBuffer::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SourceBuffer);
    Base::initialize(realm);
}

void SourceBuffer::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_media_source);
    visitor.visit(m_audio_tracks);
    visitor.visit(m_video_tracks);
    visitor.visit(m_text_tracks);
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

void SourceBuffer::set_content_type(String const& type)
{
    auto mime_type = MimeSniff::MimeType::parse(type);
    VERIFY(mime_type.has_value());

    NonnullOwnPtr<ByteStreamParser> parser = [&]() -> NonnullOwnPtr<ByteStreamParser> {
        if (mime_type->subtype() == "webm")
            return make<WebMByteStreamParser>();
        VERIFY_NOT_REACHED();
    }();

    m_processor->set_parser(move(parser));
}

// https://w3c.github.io/media-source/#dom-sourcebuffer-mode
Bindings::AppendMode SourceBuffer::mode() const
{
    return processor_mode_to_bindings(m_processor->mode());
}

// https://w3c.github.io/media-source/#dom-sourcebuffer-updating
bool SourceBuffer::updating() const
{
    return m_processor->updating();
}

// https://w3c.github.io/media-source/#dom-sourcebuffer-buffered
GC::Ref<HTML::TimeRanges> SourceBuffer::buffered()
{
    auto time_ranges = realm().create<HTML::TimeRanges>(realm());

    // FIXME: 1. If this object has been removed from the sourceBuffers attribute of the parent media source then throw
    //           an InvalidStateError exception and abort these steps.

    // NB: Further steps to intersect the buffered ranges of the track buffers are implemented within
    //     SourceBufferProcessor::buffered_ranges() below, since it has access to the track buffers.
    auto ranges = m_processor->buffered_ranges();
    for (auto const& range : ranges)
        time_ranges->add_range(range.start.to_seconds_f64(), range.end.to_seconds_f64());

    return time_ranges;
}

// https://w3c.github.io/media-source/#dom-sourcebuffer-mode
WebIDL::ExceptionOr<void> SourceBuffer::set_mode(Bindings::AppendMode mode)
{
    // 1. If this object has been removed from the sourceBuffers attribute of the parent media source
    //    then throw an InvalidStateError exception and abort these steps.
    if (!m_media_source->source_buffers()->contains(*this))
        return WebIDL::InvalidStateError::create(realm(), "SourceBuffer has been removed"_utf16);

    // 2. If the updating attribute equals true, then throw an InvalidStateError exception and abort these steps.
    if (updating())
        return WebIDL::InvalidStateError::create(realm(), "SourceBuffer is updating"_utf16);

    // 3. If the [[generate timestamps flag]] equals true and the new value equals "segments",
    //    then throw a TypeError exception and abort these steps.
    if (m_processor->generate_timestamps_flag() && mode == Bindings::AppendMode::Segments)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Cannot set mode to 'segments' when generate timestamps flag is true"sv };

    // 4. If the readyState attribute of the parent media source is in the "ended" state then run the following steps:
    if (m_media_source->ready_state() == Bindings::ReadyState::Ended) {
        // 1. Set the readyState attribute of the parent media source to "open"
        // 2. Queue a task to fire an event named sourceopen at the parent media source.
        m_media_source->set_ready_state_to_open_and_fire_sourceopen_event();
    }

    // 5. If the [[append state]] equals PARSING_MEDIA_SEGMENT, then throw an InvalidStateError exception
    //    and abort these steps.
    if (m_processor->is_parsing_media_segment())
        return WebIDL::InvalidStateError::create(realm(), "Cannot change mode while parsing a media segment"_utf16);

    // 6. If the new value equals "sequence", then set the [[group start timestamp]] to the [[group end timestamp]].
    if (mode == Bindings::AppendMode::Sequence)
        m_processor->set_group_start_timestamp(m_processor->group_end_timestamp());

    // 7. Update the attribute to the new value.
    m_processor->set_mode(bindings_mode_to_processor(mode));

    return {};
}

// https://w3c.github.io/media-source/#sourcebuffer-prepare-append
WebIDL::ExceptionOr<void> SourceBuffer::prepare_append()
{
    // FIXME: Support MediaSourceExtensions in workers.
    if (!m_media_source->media_element_assigned_to())
        return WebIDL::InvalidStateError::create(realm(), "Unsupported in workers"_utf16);

    // 1. If the SourceBuffer has been removed from the sourceBuffers attribute of the parent media source then throw an
    //    InvalidStateError exception and abort these steps.
    if (!m_media_source->source_buffers()->contains(*this))
        return WebIDL::InvalidStateError::create(realm(), "SourceBuffer has been removed"_utf16);

    // 2. If the updating attribute equals true, then throw an InvalidStateError exception and abort these steps.
    if (updating())
        return WebIDL::InvalidStateError::create(realm(), "SourceBuffer is already updating"_utf16);

    // 3. Let recent element error be determined as follows:
    auto recent_element_error = [&] {
        // If the MediaSource was constructed in a Window
        if (m_media_source->media_element_assigned_to()) {
            // Let recent element error be true if the HTMLMediaElement's error attribute is not null.
            // If that attribute is null, then let recent element error be false.
            return m_media_source->media_element_assigned_to()->error() != nullptr;
        }
        // Otherwise
        // FIXME: Let recent element error be the value resulting from the steps for the Window case,
        //        but run on the Window HTMLMediaElement on any change to its error attribute and
        //        communicated by using [[port to worker]] implicit messages.
        //        If such a message has not yet been received, then let recent element error be false.
        VERIFY_NOT_REACHED();
    }();

    // 4. If recent element error is true, then throw an InvalidStateError exception and abort these steps.
    if (recent_element_error)
        return WebIDL::InvalidStateError::create(realm(), "Element has a recent error"_utf16);

    // 5. If the readyState attribute of the parent media source is in the "ended" state then run the following steps:
    if (m_media_source->ready_state() == Bindings::ReadyState::Ended) {
        // 1. Set the readyState attribute of the parent media source to "open"
        // 2. Queue a task to fire an event named sourceopen at the parent media source.
        m_media_source->set_ready_state_to_open_and_fire_sourceopen_event();
    }

    // 6. Run the coded frame eviction algorithm.
    m_processor->run_coded_frame_eviction();

    // 7. If the [[buffer full flag]] equals true, then throw a QuotaExceededError exception and abort these steps.
    if (m_processor->is_buffer_full())
        return WebIDL::QuotaExceededError::create(realm(), "Buffer is full"_utf16);

    return {};
}

// https://w3c.github.io/media-source/#dom-sourcebuffer-appendbuffer
WebIDL::ExceptionOr<void> SourceBuffer::append_buffer(GC::Root<WebIDL::BufferSource> const& data)
{
    // 1. Run the prepare append algorithm.
    TRY(prepare_append());

    // 2. Add data to the end of the [[input buffer]].
    if (auto array_buffer = data->viewed_array_buffer(); array_buffer && !array_buffer->is_detached()) {
        auto bytes = array_buffer->buffer().bytes().slice(data->byte_offset(), data->byte_length());
        m_processor->append_to_input_buffer(bytes);
    }

    // 3. Set the updating attribute to true.
    m_processor->set_updating(true);

    // 4. Queue a task to fire an event named updatestart at this SourceBuffer object.
    m_media_source->queue_a_media_source_task(GC::create_function(heap(), [this] {
        dispatch_event(DOM::Event::create(realm(), EventNames::updatestart));
    }));

    // 5. Asynchronously run the buffer append algorithm.
    m_media_source->queue_a_media_source_task(GC::create_function(heap(), [this] {
        run_buffer_append_algorithm();
    }));

    return {};
}

// https://w3c.github.io/media-source/#dom-sourcebuffer-abort
WebIDL::ExceptionOr<void> SourceBuffer::abort()
{
    // 1. If this object has been removed from the sourceBuffers attribute of the parent media source
    //    then throw an InvalidStateError exception and abort these steps.
    if (!m_media_source->source_buffers()->contains(*this))
        return WebIDL::InvalidStateError::create(realm(), "SourceBuffer has been removed"_utf16);

    // 2. If the readyState attribute of the parent media source is not in the "open" state
    //    then throw an InvalidStateError exception and abort these steps.
    if (m_media_source->ready_state() != Bindings::ReadyState::Open)
        return WebIDL::InvalidStateError::create(realm(), "MediaSource is not open"_utf16);

    // FIXME: 3. If the range removal algorithm is running, then throw an InvalidStateError exception and abort these steps.

    // 4. If the updating attribute equals true, then run the following steps:
    if (updating()) {
        // 4.1. Abort the buffer append algorithm if it is running.
        // FIXME: The buffer append algorithm cannot be running in parallel with this code. However, when
        //        it can, this will need additional work.

        // 4.2. Set the updating attribute to false.
        m_processor->set_updating(false);

        // 4.3. Queue a task to fire an event named abort at this SourceBuffer object.
        m_media_source->queue_a_media_source_task(GC::create_function(heap(), [this] {
            dispatch_event(DOM::Event::create(realm(), EventNames::abort));
        }));

        // 4.4. Queue a task to fire an event named updateend at this SourceBuffer object.
        m_media_source->queue_a_media_source_task(GC::create_function(heap(), [this] {
            dispatch_event(DOM::Event::create(realm(), EventNames::updateend));
        }));
    }

    // 5. Run the reset parser state algorithm.
    m_processor->reset_parser_state();

    // FIXME: 6. Set appendWindowStart to the presentation start time.
    //        7. Set appendWindowEnd to positive Infinity.

    return {};
}

// https://w3c.github.io/media-source/#dom-sourcebuffer-changetype
WebIDL::ExceptionOr<void> SourceBuffer::change_type(String const& type)
{
    // 1. If type is an empty string then throw a TypeError exception and abort these steps.
    if (type.is_empty())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Type cannot be empty"sv };

    // 2. If this object has been removed from the sourceBuffers attribute of the parent media source,
    //    then throw an InvalidStateError exception and abort these steps.
    if (!m_media_source->source_buffers()->contains(*this))
        return WebIDL::InvalidStateError::create(realm(), "SourceBuffer has been removed"_utf16);

    // 3. If the updating attribute equals true, then throw an InvalidStateError exception and abort these steps.
    if (updating())
        return WebIDL::InvalidStateError::create(realm(), "SourceBuffer is updating"_utf16);

    // 4. If type contains a MIME type that is not supported or contains a MIME type that is not supported
    //    with the types specified (currently or previously) of SourceBuffer objects in the sourceBuffers
    //    attribute of the parent media source, then throw a NotSupportedError exception and abort these steps.
    if (!MediaSource::is_type_supported(type))
        return WebIDL::NotSupportedError::create(realm(), "Type is not supported"_utf16);

    // 5. If the readyState attribute of the parent media source is in the "ended" state then run the following steps:
    if (m_media_source->ready_state() == Bindings::ReadyState::Ended) {
        // 5.1. Set the readyState attribute of the parent media source to "open"
        // 5.2. Queue a task to fire an event named sourceopen at the parent media source.
        m_media_source->set_ready_state_to_open_and_fire_sourceopen_event();
    }

    // 6. Run the reset parser state algorithm.
    m_processor->reset_parser_state();

    // AD-HOC: Recreate the byte stream parser for the new type.
    set_content_type(type);

    // 7. Update the [[generate timestamps flag]] on this SourceBuffer object to the value in the
    //    "Generate Timestamps Flag" column of the byte stream format registry entry that is associated with type.
    // FIXME: Look up the generate timestamps flag from the registry
    // For now, assume false for most formats
    m_processor->set_generate_timestamps_flag(false);

    // 8. If the [[generate timestamps flag]] equals true:
    //       Set the mode attribute on this SourceBuffer object to "sequence", including running the
    //       associated steps for that attribute being set.
    //    Otherwise:
    //       Keep the previous value of the mode attribute on this SourceBuffer object, without running
    //       any associated steps for that attribute being set.
    if (m_processor->generate_timestamps_flag())
        TRY(set_mode(Bindings::AppendMode::Sequence));

    // 9. Set the [[pending initialization segment for changeType flag]] on this SourceBuffer object to true.
    m_processor->set_pending_initialization_segment_for_change_type_flag(true);

    return {};
}

void SourceBuffer::set_reached_end_of_stream(Badge<MediaSource>)
{
    m_processor->set_reached_end_of_stream();
}

void SourceBuffer::clear_reached_end_of_stream(Badge<MediaSource>)
{
    m_processor->clear_reached_end_of_stream();
}

// https://w3c.github.io/media-source/#sourcebuffer-buffer-append
void SourceBuffer::run_buffer_append_algorithm()
{
    // 1. Run the segment parser loop algorithm.
    // NB: SourceBufferProcessor's append done callback invokes finish_buffer_append for the rest of this algorithm.
    m_processor->run_segment_parser_loop();
}

// https://w3c.github.io/media-source/#sourcebuffer-append-error
void SourceBuffer::run_append_error_algorithm()
{
    // 1. Run the reset parser state algorithm.
    m_processor->reset_parser_state();

    // 2. Set the updating attribute to false.
    m_processor->set_updating(false);

    // 3. Queue a task to fire an event named error at this SourceBuffer object.
    m_media_source->queue_a_media_source_task(GC::create_function(heap(), [this] {
        dispatch_event(DOM::Event::create(realm(), EventNames::error));
    }));

    // 4. Queue a task to fire an event named updateend at this SourceBuffer object.
    m_media_source->queue_a_media_source_task(GC::create_function(heap(), [this] {
        dispatch_event(DOM::Event::create(realm(), EventNames::updateend));
    }));

    // 5. Run the end of stream algorithm with the error parameter set to "decode".
    m_media_source->run_end_of_stream_algorithm({}, Bindings::EndOfStreamError::Decode);
}

// https://w3c.github.io/media-source/#sourcebuffer-init-segment-received
void SourceBuffer::on_first_initialization_segment_processed(InitializationSegmentData const& init_data)
{
    auto& realm = this->realm();

    // 4. Let active track flag equal false.
    bool active_track_flag = false;

    // 5. If the [[first initialization segment received flag]] is false, then run the following steps:
    {
        // FIXME: 1. If the initialization segment contains tracks with codecs the user agent does not support,
        //           then run the append error algorithm and abort these steps.

        // 2. For each audio track in the initialization segment, run following steps:
        for (auto const& audio_track_info : init_data.audio_tracks) {
            auto const& audio_track = audio_track_info.track;

            // 1. Let audio byte stream track ID be the Track ID for the current track being processed.
            // NB: Used by the processor when creating the track buffer.

            // 2. Let audio language be a BCP 47 language tag for the language specified in the initialization
            //    segment for this track or an empty string if no language info is present.
            // 3. If audio language equals the 'und' BCP 47 value, then assign an empty string to audio language.
            // 4. Let audio label be a label specified in the initialization segment for this track or an empty
            //    string if no label info is present.
            // NB: All of the above is handled by the MediaTrackBase constructor.

            // 5. Let audio kinds be a sequence of kind strings specified in the initialization segment for this
            //    track or a sequence with a single empty string element in it if no kind information is provided.
            Array audio_kinds = { audio_track.kind() };

            // 6. For each value in audio kinds, run the following steps:
            for (auto const& current_audio_kind : audio_kinds) {
                // 1. Let current audio kind equal the value from audio kinds for this iteration of the loop.
                // 2. Let new audio track be a new AudioTrack object.
                auto new_audio_track = realm.create<HTML::AudioTrack>(realm, *m_media_source->media_element_assigned_to(), audio_track);
                // 3. Generate a unique ID and assign it to the id property on new audio track.
                auto unique_id = m_media_source->next_track_id();
                new_audio_track->set_id(unique_id);

                // 4. Assign audio language to the language property on new audio track.
                // 5. Assign audio label to the label property on new audio track.

                // 6. Assign current audio kind to the kind property on new audio track.
                new_audio_track->set_kind(current_audio_kind);

                // 7. If this SourceBuffer object's audioTracks's length equals 0, then run the following steps:
                if (m_audio_tracks->length() == 0) {
                    // 1. Set the enabled property on new audio track to true.
                    new_audio_track->set_enabled(true);
                    // 2. Set active track flag to true.
                    active_track_flag = true;
                }

                // 8. Add new audio track to the audioTracks attribute on this SourceBuffer object.
                m_audio_tracks->add_track(new_audio_track);

                // 9. If the parent media source was constructed in a DedicatedWorkerGlobalScope:
                if (!m_media_source->media_element_assigned_to()) {
                    // FIXME: Post an internal `create track mirror` message...
                    VERIFY_NOT_REACHED();
                }
                // Otherwise:
                // Add new audio track to the audioTracks attribute on the HTMLMediaElement.
                m_media_source->media_element_assigned_to()->audio_tracks()->add_track(new_audio_track);
            }

            // 7. Create a new track buffer to store coded frames for this track.
            // 8. Add the track description for this track to the track buffer.
            // NB: Track buffers and their demuxers are created by the processor. Here we pass the
            //     demuxer to the PlaybackManager so the decoder thread can read coded frames from it.
            m_media_source->media_element_assigned_to()->playback_manager().add_media_source(audio_track_info.demuxer);
        }

        // 3. For each video track in the initialization segment, run following steps:
        for (auto const& video_track_info : init_data.video_tracks) {
            auto const& video_track = video_track_info.track;

            // 1. Let video byte stream track ID be the Track ID for the current track being processed.
            // NB: Used by the processor when creating the track buffer.

            // 2. Let video language be a BCP 47 language tag for the language specified in the initialization
            //    segment for this track or an empty string if no language info is present.
            // 3. If video language equals the 'und' BCP 47 value, then assign an empty string to video language.
            // 4. Let video label be a label specified in the initialization segment for this track or an empty
            //    string if no label info is present.
            // NB: All of the above is handled by the MediaTrackBase constructor.

            // 5. Let video kinds be a sequence of kind strings specified in the initialization segment for this
            //    track or a sequence with a single empty string element in it if no kind information is provided.
            Array video_kinds = { video_track.kind() };

            // 6. For each value in video kinds, run the following steps:
            for (auto const& current_video_kind : video_kinds) {
                // 1. Let current video kind equal the value from video kinds for this iteration of the loop.
                // 2. Let new video track be a new VideoTrack object.
                auto new_video_track = realm.create<HTML::VideoTrack>(realm, *m_media_source->media_element_assigned_to(), video_track);
                // 3. Generate a unique ID and assign it to the id property on new video track.
                auto unique_id = m_media_source->next_track_id();
                new_video_track->set_id(unique_id);

                // 4. Assign video language to the language property on new video track.
                // 5. Assign video label to the label property on new video track.

                // 6. Assign current video kind to the kind property on new video track.
                new_video_track->set_kind(current_video_kind);

                // 7. If this SourceBuffer object's videoTracks's length equals 0, then run the following steps:
                if (m_video_tracks->length() == 0) {
                    // 1. Set the selected property on new video track to true.
                    new_video_track->set_selected(true);
                    // 2. Set active track flag to true.
                    active_track_flag = true;
                }

                // 8. Add new video track to the videoTracks attribute on this SourceBuffer object.
                m_video_tracks->add_track(new_video_track);

                // 9. If the parent media source was constructed in a DedicatedWorkerGlobalScope:
                if (!m_media_source->media_element_assigned_to()) {
                    // FIXME: Post an internal `create track mirror` message...
                    VERIFY_NOT_REACHED();
                }
                // Otherwise:
                // Add new video track to the videoTracks attribute on the HTMLMediaElement.
                m_media_source->media_element_assigned_to()->video_tracks()->add_track(new_video_track);
            }

            // 7. Create a new track buffer to store coded frames for this track.
            // 8. Add the track description for this track to the track buffer.
            // NB: Track buffers and their demuxers are created by the processor. Here we pass the
            //     demuxer to the PlaybackManager so the decoder thread can read coded frames from it.
            m_media_source->media_element_assigned_to()->playback_manager().add_media_source(video_track_info.demuxer);
        }

        // 4. For each text track in the initialization segment, run following steps:
        for (auto const& text_track_info : init_data.text_tracks) {
            auto const& text_track = text_track_info.track;

            // 1. Let text byte stream track ID be the Track ID for the current track being processed.
            // NB: Used by the processor when creating the track buffer.

            // 2. Let text language be a BCP 47 language tag for the language specified in the initialization
            //    segment for this track or an empty string if no language info is present.
            auto text_language = text_track.language();
            // 3. If text language equals the 'und' BCP 47 value, then assign an empty string to text language.
            if (text_language == u"und"sv)
                text_language = ""_utf16;

            // 4. Let text label be a label specified in the initialization segment for this track or an empty
            //    string if no label info is present.
            auto const& text_label = text_track.label();

            // 5. Let text kinds be a sequence of kind strings specified in the initialization segment for this
            //    track or a sequence with a single empty string element in it if no kind information is provided.
            Array text_kinds = { text_track.kind() };

            // 6. For each value in text kinds, run the following steps:
            for (auto const& current_text_kind : text_kinds) {
                // 1. Let current text kind equal the value from text kinds for this iteration of the loop.
                // 2. Let new text track be a new TextTrack object.
                auto new_text_track = realm.create<HTML::TextTrack>(realm);
                // 3. Generate a unique ID and assign it to the id property on new text track.
                auto unique_id = m_media_source->next_track_id();
                new_text_track->set_id(unique_id.to_utf8());

                // 4. Assign text language to the language property on new text track.
                new_text_track->set_language(text_language.to_utf8());

                // 5. Assign text label to the label property on new text track.
                new_text_track->set_label(text_label.to_utf8());

                // 6. Assign current text kind to the kind property on new text track.
                new_text_track->set_kind(media_track_kind_to_text_track_kind(current_text_kind));

                // FIXME: 7. Populate the remaining properties on new text track with the appropriate information from
                //           the initialization segment.

                // FIXME: 8. If the mode property on new text track equals "showing" or "hidden", then set active track
                //           flag to true.

                // 9. Add new text track to the textTracks attribute on this SourceBuffer object.
                m_text_tracks->add_track(new_text_track);

                // 10. If the parent media source was constructed in a DedicatedWorkerGlobalScope:
                if (!m_media_source->media_element_assigned_to()) {
                    // FIXME: Post an internal `create track mirror` message...
                    VERIFY_NOT_REACHED();
                }
                // Otherwise:
                // Add new text track to the textTracks attribute on the HTMLMediaElement.
                m_media_source->media_element_assigned_to()->text_tracks()->add_track(new_text_track);
            }

            // 7. Create a new track buffer to store coded frames for this track.
            // 8. Add the track description for this track to the track buffer.
            // NB: Track buffers and their demuxers are created by the processor.
            //     Text track demuxers are not added to the PlaybackManager.
        }

        // 5. If active track flag equals true, then run the following steps:
        if (active_track_flag) {
            // 1. Add this SourceBuffer to activeSourceBuffers.
            m_media_source->active_source_buffers()->append(*this);

            // 2. Queue a task to fire an event named addsourcebuffer at activeSourceBuffers.
            m_media_source->queue_a_media_source_task(GC::create_function(heap(), [realm = GC::Ref(realm), source_buffers = m_media_source->active_source_buffers()] {
                source_buffers->dispatch_event(DOM::Event::create(realm, EventNames::addsourcebuffer));
            }));
        }

        // 6. Set [[first initialization segment received flag]] to true.
        // AD-HOC: This is handled within SourceBufferProcessor, since it's observable immediately in the processing
        //         loop.
    }

    // 6. Set [[pending initialization segment for changeType flag]] to false.
    // AD-HOC: This is handled within SourceBufferProcessor, same as the above inner step 6.

    // 7. If the active track flag equals true, then run the following steps:
    if (!active_track_flag)
        return;

    // FIXME: Mirror the following steps to the Window when workers are supported.
    auto& media_element = *m_media_source->media_element_assigned_to();

    // 8. Use the parent media source's mirror if necessary algorithm to run the following step in Window:
    //        If the HTMLMediaElement's readyState attribute is greater than HAVE_CURRENT_DATA, then set
    //        the HTMLMediaElement's readyState attribute to HAVE_METADATA.
    // 9. If each object in sourceBuffers of the parent media source has [[first initialization segment received
    //    flag]] equal to true, then use the parent media source's mirror if necessary algorithm to run the
    //    following step in Window:
    //        If the HTMLMediaElement's readyState attribute is HAVE_NOTHING, then set the HTMLMediaElement's
    //        readyState attribute to HAVE_METADATA.
    // NB: These steps are handled by the unified readyState update method on HTMLMediaElement, based on conditions
    //     that Media Source Extensions requires.
    media_element.update_ready_state();
}

// https://w3c.github.io/media-source/#sourcebuffer-buffer-append
void SourceBuffer::finish_buffer_append()
{
    // 3. Set the updating attribute to false.
    m_processor->set_updating(false);

    // 4. Queue a task to fire an event named update at this SourceBuffer object.
    m_media_source->queue_a_media_source_task(GC::create_function(heap(), [this] {
        dispatch_event(DOM::Event::create(realm(), EventNames::update));
    }));

    // 5. Queue a task to fire an event named updateend at this SourceBuffer object.
    m_media_source->queue_a_media_source_task(GC::create_function(heap(), [this] {
        dispatch_event(DOM::Event::create(realm(), EventNames::updateend));
    }));
}

// https://w3c.github.io/media-source/#sourcebuffer-coded-frame-processing
void SourceBuffer::update_ready_state_and_duration_after_coded_frame_processing()
{
    auto media_element = m_media_source->media_element_assigned_to();
    VERIFY(media_element);

    // AD-HOC: Steps 2-4 (readyState transitions based on new coded frames) are covered by the unified
    //         readyState update method on HTMLMediaElement.
    media_element->update_ready_state();

    // FIXME: 5. If the media segment contains data beyond the current duration, then run the duration change
    //           algorithm with new duration set to the maximum of the current duration and the group end timestamp.
}

}
