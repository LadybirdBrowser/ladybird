/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <LibCore/EventLoop.h>
#include <LibGfx/YUVData.h>
#include <LibMedia/DecoderRegistry.h>
#include <LibMedia/Demuxer.h>
#include <LibMedia/Sinks/VideoSink.h>
#include <LibMedia/VideoDecoder.h>
#include <LibMedia/VideoFrame.h>
#include <LibThreading/Thread.h>

#include "DecodedVideoProducer.h"

namespace Media {

namespace {

DecoderErrorOr<FramePlaneLayout> plane_layout_for_frame(VideoFrameMetadata const& metadata)
{
    auto layout_result = frame_plane_layout(metadata.size, metadata.bit_depth, metadata.subsampling);
    if (layout_result.is_error())
        return DecoderError::format(DecoderErrorCategory::Invalid, "Failed to compute video frame plane layout: {}", layout_result.release_error());
    return layout_result.release_value();
}

}

DecoderErrorOr<NonnullRefPtr<DecodedVideoProducer>> DecodedVideoProducer::try_create(Core::EventLoop& main_thread_event_loop, NonnullRefPtr<Demuxer> const& demuxer, Track const& track, AK::Duration auto_suspend_idle_timeout)
{
    TRY(demuxer->create_context_for_track(track));
    auto thread_data = DECODER_TRY_ALLOC(try_make_ref_counted<DecodedVideoProducer::ThreadData>(main_thread_event_loop, demuxer, track, auto_suspend_idle_timeout));
    auto producer = DECODER_TRY_ALLOC(try_make_ref_counted<DecodedVideoProducer>(thread_data));

    auto thread = DECODER_TRY_ALLOC(Threading::Thread::try_create("Video Decoder"sv, [thread_data]() -> int {
        thread_data->register_decode_thread();
        thread_data->wait_for_start();
        while (!thread_data->should_thread_exit()) {
            if (thread_data->handle_auto_suspension())
                continue;
            thread_data->handle_seek();
            thread_data->push_data_and_decode_some_frames();
        }
        thread_data->release_decoder();
        return 0;
    }));
    thread->start();
    thread->detach();

    return producer;
}

DecodedVideoProducer::DecodedVideoProducer(NonnullRefPtr<ThreadData> const& thread_state)
    : m_thread_data(thread_state)
{
}

DecodedVideoProducer::~DecodedVideoProducer()
{
    m_thread_data->exit();
}

void DecodedVideoProducer::set_error_handler(ErrorHandler&& handler)
{
    m_thread_data->set_error_handler(move(handler));
}

void DecodedVideoProducer::set_read_blocked_change_handler(ReadBlockedChangeHandler handler)
{
    m_thread_data->set_read_blocked_change_handler(move(handler));
}

void DecodedVideoProducer::start()
{
    m_thread_data->start();
}

VideoProducerOutput DecodedVideoProducer::peek()
{
    return m_thread_data->peek();
}

void DecodedVideoProducer::consume()
{
    m_thread_data->consume();
}

void DecodedVideoProducer::set_wake_handler(PipelineWakeHandler handler)
{
    m_thread_data->set_wake_handler(move(handler));
}

VideoProducerOutput DecodedVideoProducer::ThreadData::peek()
{
    auto locker = take_lock();
    VERIFY(!m_decode_thread_id.is_current_thread());
    note_consumer_activity_while_locked();
    auto output = peek_while_locked();
    m_downstream_needs_wake = is_waiting_for_data(output.status);
    return output;
}

VideoProducerOutput DecodedVideoProducer::ThreadData::peek_while_locked()
{
    if (m_last_processed_seek_id != m_seek_id)
        return { nullptr, PipelineStatus::Pending };
    if (!m_queue.is_empty())
        return { m_queue.head().frame, PipelineStatus::HaveData };
    return { nullptr, m_current_halting_status };
}

void DecodedVideoProducer::ThreadData::consume()
{
    // Hold the frame until after the mutex is unlocked, or our pool slot freed callback may reentrantly lock.
    RefPtr<VideoFrame> frame;

    auto locker = take_lock();
    VERIFY(!m_decode_thread_id.is_current_thread());
    note_consumer_activity_while_locked();
    if (m_queue.is_empty())
        return;
    frame = m_queue.dequeue().frame;
    m_earliest_available_timestamp = max(m_earliest_available_timestamp, frame->timestamp() + frame->duration());
    wake();
}

void DecodedVideoProducer::ThreadData::enter_halting_state(PipelineStatus status, Optional<DecoderError> error)
{
    VERIFY(!error.has_value() || error->category() != DecoderErrorCategory::Aborted);

    VERIFY(status == PipelineStatus::EndOfStream || status == PipelineStatus::Error);
    m_current_halting_status = status;
    dispatch_wake_if_needed_while_locked();
    if (error.has_value()) {
        invoke_on_main_thread_while_locked([error = error.release_value()](auto const& self) mutable {
            self->dispatch_error(move(error));
        });
    }
}

void DecodedVideoProducer::ThreadData::set_wake_handler(PipelineWakeHandler handler)
{
    auto locker = take_lock();
    m_wake_handler = move(handler);
}

void DecodedVideoProducer::ThreadData::dispatch_wake_if_needed_while_locked()
{
    if (!m_downstream_needs_wake)
        return;
    auto seek_id = m_seek_id.load();
    invoke_on_main_thread_while_locked([seek_id](auto& self) {
        if (self->m_seek_id != seek_id)
            return;
        if (self->m_wake_handler)
            self->m_wake_handler();
    });
    m_downstream_needs_wake = false;
}

AK::Duration DecodedVideoProducer::select_fast_seek_target(AK::Duration timestamp, SeekMode mode)
{
    return m_thread_data->select_fast_seek_target(timestamp, mode);
}

void DecodedVideoProducer::seek(AK::Duration timestamp)
{
    m_thread_data->seek(timestamp);
}

DecodedVideoProducer::ThreadData::ThreadData(Core::EventLoop& main_thread_event_loop, NonnullRefPtr<Demuxer> const& demuxer, Track const& track, AK::Duration auto_suspend_idle_timeout)
    : m_main_thread_event_loop(main_thread_event_loop)
    , m_demuxer(demuxer)
    , m_track(track)
    , m_auto_suspend_idle_timeout(auto_suspend_idle_timeout)
{
    m_demuxer->set_read_blocked_change_handler_for_track(m_track, [this](ReadBlocked blocked) {
        dispatch_read_blocked_change(blocked);
    });
}

DecoderErrorOr<void> DecodedVideoProducer::ThreadData::create_decoder_for_frame(CodedFrame const& frame)
{
    auto codec_initialization_data = frame.new_codec_configuration();
    if (!codec_initialization_data.has_value())
        return DecoderError::with_description(DecoderErrorCategory::Corrupted, "Coded frame starting a decode sequence carries no codec configuration"sv);
    m_decoder = TRY(create_video_decoder(frame.codec_id(), *codec_initialization_data));
    m_decoder_codec_id = frame.codec_id();
    return {};
}

DecoderErrorOr<void> DecodedVideoProducer::ThreadData::receive_coded_frame(CodedFrame const& frame)
{
    VERIFY(!m_frame_awaiting_decoder_replacement.has_value());
    if (m_decoder == nullptr) [[unlikely]] {
        TRY(create_decoder_for_frame(frame));
    } else if (m_decoder_codec_id != frame.codec_id()) [[unlikely]] {
        m_frame_awaiting_decoder_replacement = frame;
        m_decoder->signal_end_of_stream();
        return {};
    }
    return m_decoder->receive_coded_data(frame);
}

DecoderErrorOr<bool> DecodedVideoProducer::ThreadData::replace_drained_decoder()
{
    if (!m_frame_awaiting_decoder_replacement.has_value())
        return false;
    auto frame = m_frame_awaiting_decoder_replacement.release_value();
    TRY(create_decoder_for_frame(frame));
    TRY(m_decoder->receive_coded_data(frame));
    return true;
}

void DecodedVideoProducer::ThreadData::release_decoder()
{
    VERIFY(m_decode_thread_id.is_current_thread());
    auto locker = take_lock();
    m_decoder.clear();
}

DecodedVideoProducer::ThreadData::~ThreadData()
{
    m_demuxer->set_read_blocked_change_handler_for_track(m_track, nullptr);
}

void DecodedVideoProducer::ThreadData::set_error_handler(ErrorHandler&& handler)
{
    m_error_handler = move(handler);
}

void DecodedVideoProducer::ThreadData::set_read_blocked_change_handler(ReadBlockedChangeHandler handler)
{
    m_read_blocked_change_handler = move(handler);
}

void DecodedVideoProducer::ThreadData::dispatch_read_blocked_change(ReadBlocked blocked)
{
    m_main_thread_event_loop.deferred_invoke([self = NonnullRefPtr(*this), blocked] {
        if (self->m_read_blocked_change_handler)
            self->m_read_blocked_change_handler(blocked);
    });
}

void DecodedVideoProducer::ThreadData::start()
{
    auto locker = take_lock();
    if (m_requested_state != RequestedState::None)
        return;
    m_requested_state = RequestedState::Running;
    m_last_consumer_activity = MonotonicTime::now();
    wake();
}

void DecodedVideoProducer::ThreadData::exit()
{
    auto locker = take_lock();
    m_requested_state = RequestedState::Exit;
    wake();
}

void DecodedVideoProducer::ThreadData::note_consumer_activity_while_locked() const
{
    m_last_consumer_activity = MonotonicTime::now();
    m_auto_suspend_requested = false;
}

void DecodedVideoProducer::ThreadData::wait_to_decode_or_auto_suspend_while_locked()
{
    if (m_requested_state != RequestedState::Running)
        return;
    if (m_auto_suspended || m_auto_suspend_requested)
        return;

    auto idle_at = m_last_consumer_activity + m_auto_suspend_idle_timeout;
    auto now = MonotonicTime::now();
    if (now < idle_at) {
        if (m_wait_state->condition.wait_for(idle_at - now))
            return;
        if (MonotonicTime::now() < idle_at)
            return;
    }

    m_auto_suspend_requested = true;
}

DecodedVideoProducer::FrameQueue& DecodedVideoProducer::ThreadData::queue()
{
    return m_queue;
}

void DecodedVideoProducer::ThreadData::seek(AK::Duration timestamp)
{
    auto locker = take_lock();
    VERIFY(!m_decode_thread_id.is_current_thread());
    note_consumer_activity_while_locked();
    m_downstream_needs_wake = true;

    auto is_within_available_range = timestamp >= m_earliest_available_timestamp
        && (timestamp < m_latest_available_timestamp || m_current_halting_status == PipelineStatus::EndOfStream);
    if (is_within_available_range) {
        dispatch_wake_if_needed_while_locked();
        return;
    }

    m_seek_id++;
    m_seek_timestamp = timestamp;
    m_earliest_available_timestamp = timestamp;
    m_latest_available_timestamp = timestamp;
    m_demuxer->set_blocking_reads_aborted_for_track(m_track);
    wake();
}

AK::Duration DecodedVideoProducer::ThreadData::select_fast_seek_target(AK::Duration target, SeekMode mode) const
{
    return m_demuxer->select_fast_seek_target_for_track(m_track, target, mode);
}

void DecodedVideoProducer::ThreadData::register_decode_thread()
{
    VERIFY(!m_decode_thread_id.is_valid());
    m_decode_thread_id = AK::ThreadID::current();
}

void DecodedVideoProducer::ThreadData::wait_for_start()
{
    auto locker = take_lock();
    while (m_requested_state == RequestedState::None)
        m_wait_state->condition.wait();
}

bool DecodedVideoProducer::ThreadData::should_thread_exit_while_locked() const
{
    return m_requested_state == RequestedState::Exit;
}

bool DecodedVideoProducer::ThreadData::should_thread_exit() const
{
    auto locker = take_lock();
    return should_thread_exit_while_locked();
}

template<typename Invokee>
void DecodedVideoProducer::ThreadData::invoke_on_main_thread_while_locked(Invokee invokee)
{
    if (m_requested_state == RequestedState::Exit)
        return;
    m_main_thread_event_loop.deferred_invoke([self = NonnullRefPtr(*this), invokee = move(invokee)] mutable {
        invokee(self);
    });
}

bool DecodedVideoProducer::ThreadData::handle_auto_suspension()
{
    VERIFY(m_decode_thread_id.is_current_thread());
    auto locker = take_lock();
    if (!m_auto_suspend_requested)
        return false;
    VERIFY(!m_auto_suspended);

    m_queue.clear();
    m_latest_available_timestamp = m_earliest_available_timestamp;
    m_decoder.clear();
    m_decoder_needs_keyframe_next_seek = true;
    m_decoder_needs_codec_configuration_next_seek = true;
    if (m_frame_pool != nullptr)
        m_frame_pool->shed_buffers();
    m_auto_suspended = true;
    m_auto_suspend_requested = false;
    m_current_halting_status = PipelineStatus::Suspended;
    // The consumer's last peek predates the suspension, so wake it regardless of what it saw.
    m_downstream_needs_wake = true;
    dispatch_wake_if_needed_while_locked();

    while (true) {
        if (m_requested_state == RequestedState::Exit)
            return true;
        if (m_last_processed_seek_id != m_seek_id)
            break;
        m_wait_state->condition.wait();
    }
    m_auto_suspended = false;
    m_current_halting_status = PipelineStatus::Pending;

    return true;
}

void DecodedVideoProducer::ThreadData::queue_frame(NonnullRefPtr<VideoFrame> const& frame)
{
    if (m_seek_id.load() != m_last_processed_seek_id)
        return;
    m_queue.enqueue({ frame });
    m_latest_available_timestamp = max(m_latest_available_timestamp, frame->conservative_end());
    dispatch_wake_if_needed_while_locked();
}

void DecodedVideoProducer::ThreadData::dispatch_error(DecoderError&& error)
{
    if (m_error_handler)
        m_error_handler(move(error));
}

void DecodedVideoProducer::ThreadData::resolve_seek(u32 seek_id, bool moved_position)
{
    m_last_processed_seek_id = seek_id;
    if (moved_position)
        m_current_halting_status = PipelineStatus::Pending;
}

bool DecodedVideoProducer::ThreadData::handle_seek()
{
    VERIFY(m_decode_thread_id.is_current_thread());

    auto seek_id = m_seek_id.load();
    if (m_last_processed_seek_id == seek_id)
        return false;

    AK::Duration timestamp;
    bool moved_position = false;
    RefPtr<VideoFrame> last_frame;

    auto handle_error = [&](DecoderError&& error) {
        auto locker = take_lock();
        if (moved_position)
            m_current_halting_status = PipelineStatus::Pending;
        enter_halting_state(PipelineStatus::Error, move(error));
        m_last_processed_seek_id = seek_id;
    };

    while (true) {
        {
            auto locker = take_lock();
            seek_id = m_seek_id;
            timestamp = m_seek_timestamp;
            m_demuxer->reset_blocking_reads_aborted_for_track(m_track);
            m_queue.clear();
            m_frame_awaiting_decoder_replacement.clear();
        }

        auto seek_options = DemuxerSeekOptions::None;
        if (m_decoder_needs_codec_configuration_next_seek)
            seek_options |= DemuxerSeekOptions::NeedCodecConfiguration;
        if (m_decoder_needs_keyframe_next_seek)
            seek_options |= DemuxerSeekOptions::Force;
        auto demuxer_seek_result_or_error = m_demuxer->seek_to_most_recent_keyframe(m_track, timestamp, seek_options);
        if (demuxer_seek_result_or_error.is_error() && demuxer_seek_result_or_error.error().category() != DecoderErrorCategory::EndOfStream) {
            if (demuxer_seek_result_or_error.error().category() == DecoderErrorCategory::Aborted)
                continue;
            handle_error(demuxer_seek_result_or_error.release_error());
            return true;
        }

        m_decoder_needs_codec_configuration_next_seek = false;
        m_decoder_needs_keyframe_next_seek = false;

        auto demuxer_seek_result = demuxer_seek_result_or_error.value_or(DemuxerSeekResult::MovedPosition);

        if (demuxer_seek_result == DemuxerSeekResult::MovedPosition) {
            if (m_decoder != nullptr)
                m_decoder->flush();
            moved_position = true;
            last_frame.clear();
        }

        auto new_seek_id = m_seek_id.load();

        while (new_seek_id == seek_id) {
            auto coded_frame_result = m_demuxer->get_next_sample_for_track(m_track);
            if (coded_frame_result.is_error()) {
                if (coded_frame_result.error().category() == DecoderErrorCategory::EndOfStream) {
                    if (m_decoder == nullptr) {
                        auto locker = take_lock();
                        resolve_seek(seek_id, moved_position);
                        return true;
                    }
                    m_decoder->signal_end_of_stream();
                } else if (coded_frame_result.error().category() == DecoderErrorCategory::Aborted) {
                    break;
                } else {
                    handle_error(coded_frame_result.release_error());
                    return true;
                }
            } else {
                auto coded_frame = coded_frame_result.release_value();
                auto decode_result = receive_coded_frame(coded_frame);
                if (decode_result.is_error()) {
                    handle_error(decode_result.release_error());
                    return true;
                }
            }

            VERIFY(m_decoder);

            while (new_seek_id == seek_id) {
                auto metadata_result = m_decoder->peek_next_output(m_track.video_data().cicp);
                if (metadata_result.is_error()) {
                    if (metadata_result.error().category() == DecoderErrorCategory::EndOfStream) {
                        auto error_or_decoder_was_replaced = replace_drained_decoder();
                        if (error_or_decoder_was_replaced.is_error()) {
                            handle_error(error_or_decoder_was_replaced.release_error());
                            return true;
                        }
                        if (error_or_decoder_was_replaced.value())
                            continue;

                        auto locker = take_lock();
                        resolve_seek(seek_id, moved_position);
                        if (last_frame != nullptr)
                            queue_frame(last_frame.release_nonnull());
                        return true;
                    }

                    if (metadata_result.error().category() == DecoderErrorCategory::NeedsMoreInput)
                        break;

                    handle_error(metadata_result.release_error());
                    return true;
                }
                auto metadata = metadata_result.release_value();

                if (auto pool_result = ensure_frame_pool(); pool_result.is_error()) {
                    handle_error(pool_result.release_error());
                    return true;
                }

                auto layout_result = plane_layout_for_frame(metadata);
                if (layout_result.is_error()) {
                    handle_error(layout_result.release_error());
                    return true;
                }

                Optional<VideoFramePool::AcquiredSlot> acquired_slot;
                {
                    auto locker = take_lock();
                    while (true) {
                        acquired_slot = m_frame_pool->try_acquire(layout_result.value().total_byte_count);
                        if (acquired_slot.has_value())
                            break;
                        if (should_thread_exit_while_locked())
                            return true;
                        if (m_seek_id != seek_id)
                            break;
                        m_wait_state->condition.wait();
                    }
                }
                if (!acquired_slot.has_value()) {
                    new_seek_id = m_seek_id;
                    continue;
                }

                auto frame_result = take_frame_into_acquired_slot(metadata, acquired_slot.release_value());
                if (frame_result.is_error()) {
                    handle_error(frame_result.release_error());
                    return true;
                }

                auto current_frame = frame_result.release_value();
                if (current_frame->timestamp() > timestamp) {
                    auto locker = take_lock();
                    resolve_seek(seek_id, moved_position);

                    if (last_frame != nullptr)
                        queue_frame(last_frame.release_nonnull());

                    queue_frame(current_frame);
                    return true;
                }

                last_frame = move(current_frame);

                new_seek_id = m_seek_id;
            }
        }
    }
}

DecoderErrorOr<void> DecodedVideoProducer::ThreadData::ensure_frame_pool()
{
    VERIFY(m_decode_thread_id.is_current_thread());
    if (m_frame_pool != nullptr)
        return {};

    // Only the decode thread waits for slots, so a release on the decode thread itself never
    // needs a wake: its next try_acquire() comes after this release in program order.
    auto pool_result = VideoFramePool::create(
        [wait_state = m_wait_state, decode_thread_id = m_decode_thread_id] {
            if (decode_thread_id.is_current_thread())
                return;
            Sync::MutexLocker locker { wait_state->mutex };
            wait_state->condition.broadcast();
        });
    if (pool_result.is_error())
        return DecoderError::format(DecoderErrorCategory::Memory, "Failed to create a video frame pool: {}", pool_result.release_error());
    m_frame_pool = pool_result.release_value();
    return {};
}

DecoderErrorOr<NonnullRefPtr<VideoFrame>> DecodedVideoProducer::ThreadData::take_frame_into_acquired_slot(VideoFrameMetadata const& metadata, VideoFramePool::AcquiredSlot const& acquired_slot)
{
    auto pool_slot_result = m_frame_pool->try_adopt_acquired_slot(acquired_slot);
    if (pool_slot_result.is_error())
        return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to allocate a pooled frame slot reference"sv);
    auto pool_slot = pool_slot_result.release_value();

    auto layout = TRY(plane_layout_for_frame(metadata));
    auto y_data = acquired_slot.bytes.slice(0, layout.y_size);
    auto u_data = acquired_slot.bytes.slice(layout.u_offset, layout.u_size);
    auto v_data = acquired_slot.bytes.slice(layout.v_offset, layout.v_size);

    auto yuv_data = DECODER_TRY_ALLOC(Gfx::YUVData::create(metadata.size, metadata.bit_depth, metadata.subsampling, metadata.cicp, y_data, u_data, v_data));
    TRY(m_decoder->take_next_output_into(yuv_data));

    return DECODER_TRY_ALLOC(try_make_ref_counted<VideoFrame>(metadata.timestamp, metadata.duration, metadata.size.to_type<u32>(), metadata.bit_depth, yuv_data, move(pool_slot)));
}

void DecodedVideoProducer::ThreadData::push_data_and_decode_some_frames()
{
    VERIFY(m_decode_thread_id.is_current_thread());

    // FIXME: Check if the PlaybackManager's current time is ahead of the next keyframe, and seek to it if so.
    //        Demuxers currently can't report the next keyframe in a convenient way, so that will need implementing
    //        before this functionality can exist.

    auto set_halting_status_and_wait_for_seek = [this](PipelineStatus status, Optional<DecoderError> error) {
        auto locker = take_lock();
        enter_halting_state(status, move(error));

        dbgln_if(PLAYBACK_MANAGER_DEBUG, "Decoded Video Producer: Reached a halting pull status, waiting for a seek to start decoding again...");
        while (true) {
            if (m_seek_id != m_last_processed_seek_id)
                return;
            m_wait_state->condition.wait();
            if (should_thread_exit_while_locked())
                return;
        }
    };

    // If a prior seek encountered a decoding error, stop here to ensure that the pipeline state stays Error.
    if (m_current_halting_status != PipelineStatus::Pending) {
        set_halting_status_and_wait_for_seek(m_current_halting_status, {});
        return;
    }

    auto sample_result = m_demuxer->get_next_sample_for_track(m_track);
    if (sample_result.is_error()) {
        if (sample_result.error().category() == DecoderErrorCategory::EndOfStream) {
            if (m_decoder == nullptr) {
                set_halting_status_and_wait_for_seek(PipelineStatus::EndOfStream, {});
                return;
            }
            m_decoder->signal_end_of_stream();
        } else if (sample_result.error().category() == DecoderErrorCategory::Aborted) {
            return;
        } else {
            set_halting_status_and_wait_for_seek(PipelineStatus::Error, sample_result.release_error());
            return;
        }
    } else {
        auto coded_frame = sample_result.release_value();
        auto decode_result = receive_coded_frame(coded_frame);
        if (decode_result.is_error()) {
            set_halting_status_and_wait_for_seek(PipelineStatus::Error, decode_result.release_error());
            return;
        }
    }

    VERIFY(m_decoder);

    while (true) {
        auto metadata_result = m_decoder->peek_next_output(m_track.video_data().cicp);
        if (metadata_result.is_error()) {
            if (metadata_result.error().category() == DecoderErrorCategory::NeedsMoreInput)
                break;
            if (metadata_result.error().category() == DecoderErrorCategory::EndOfStream) {
                auto replaced_result = replace_drained_decoder();
                if (replaced_result.is_error()) {
                    set_halting_status_and_wait_for_seek(PipelineStatus::Error, replaced_result.release_error());
                    break;
                }
                if (replaced_result.value())
                    continue;
                set_halting_status_and_wait_for_seek(PipelineStatus::EndOfStream, {});
            } else
                set_halting_status_and_wait_for_seek(PipelineStatus::Error, metadata_result.release_error());
            break;
        }
        auto metadata = metadata_result.release_value();

        if (auto pool_result = ensure_frame_pool(); pool_result.is_error()) {
            set_halting_status_and_wait_for_seek(PipelineStatus::Error, pool_result.release_error());
            break;
        }

        auto layout_result = plane_layout_for_frame(metadata);
        if (layout_result.is_error()) {
            set_halting_status_and_wait_for_seek(PipelineStatus::Error, layout_result.release_error());
            break;
        }

        Optional<VideoFramePool::AcquiredSlot> acquired_slot;
        while (true) {
            if (handle_seek())
                return;

            if (handle_auto_suspension())
                return;

            auto locker = take_lock();
            acquired_slot = m_frame_pool->try_acquire(layout_result.value().total_byte_count);
            if (acquired_slot.has_value())
                break;
            wait_to_decode_or_auto_suspend_while_locked();
            if (should_thread_exit_while_locked())
                return;
        }

        auto frame_result = take_frame_into_acquired_slot(metadata, acquired_slot.release_value());
        if (frame_result.is_error()) {
            set_halting_status_and_wait_for_seek(PipelineStatus::Error, frame_result.release_error());
            break;
        }
        auto frame = frame_result.release_value();

        auto locker = take_lock();
        queue_frame(frame);
    }
}

}
