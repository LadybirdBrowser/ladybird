/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibMedia/Demuxer.h>
#include <LibMedia/PipelineStatus.h>
#include <LibMedia/Sinks/VideoSink.h>
#include <LibMediaCodecs/VideoDecoder.h>
#include <LibMediaCodecs/VideoDecoderRegistry.h>
#include <LibMediaCodecs/VideoFrame.h>
#include <LibThreading/Thread.h>

#include "DecodedVideoProducer.h"

namespace Media {

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
    m_wake_handler.set(move(handler));
}

void DecodedVideoProducer::ThreadData::dispatch_wake_if_needed_while_locked()
{
    if (!m_downstream_needs_wake)
        return;
    auto seek_id = m_seek_id.load();
    invoke_on_main_thread_while_locked([seek_id](auto& self) {
        if (self->m_seek_id != seek_id)
            return;
        self->m_wake_handler.dispatch();
    });
    m_downstream_needs_wake = false;
}

AK::Duration DecodedVideoProducer::select_fast_seek_target(AK::Duration timestamp, SeekMode mode)
{
    return m_thread_data->select_fast_seek_target(timestamp, mode);
}

void DecodedVideoProducer::seek(AK::Duration timestamp, InvalidateHeldData invalidate_held_data)
{
    m_thread_data->seek(timestamp, invalidate_held_data);
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

VideoDecoderSelection DecodedVideoProducer::ThreadData::select_decoder_for_frame(CodedFrame const& frame, VideoDecoderSelection after) const
{
    return select_video_decoder(parsed_codec_for_coded_frame(frame, m_track.parsed_codec()), after);
}

void DecodedVideoProducer::ThreadData::replace_decoder_once_drained(CodedFrame const& frame, DecodeIntent intent)
{
    m_frame_awaiting_decoder_replacement = frame;
    m_intent_awaiting_decoder_replacement = intent;
    m_decoder->signal_end_of_stream();
}

DecoderErrorOr<void> DecodedVideoProducer::ThreadData::receive_into_decoder(CodedFrame const& frame, DecodeIntent intent)
{
    if (m_non_reference_frame_detector.frame_is_non_reference(frame) && intent == DecodeIntent::Reference)
        return {};

    auto receive_result = m_decoder->receive_coded_data(frame, intent);

    if (receive_result.is_error() && receive_result.error().category() == DecoderErrorCategory::NotImplemented) {
        m_decoder_that_failed_due_to_missing_features = m_decoder_selection;
        replace_decoder_once_drained(frame, intent);
        return {};
    }

    if (receive_result.is_error() && receive_result.error().category() == DecoderErrorCategory::EndOfStream) {
        dbgln("DecodedVideoProducer: Decoder refused a frame at {} after reaching EOS", frame.presentation_timestamp());
        return {};
    }

    return receive_result;
}

DecoderErrorOr<void> DecodedVideoProducer::ThreadData::create_decoder_for_frame(CodedFrame const& frame)
{
    auto codec_initialization_data = frame.new_codec_configuration();
    if (!codec_initialization_data.has_value())
        return DecoderError::with_description(DecoderErrorCategory::Corrupted, "Coded frame starting a decode sequence carries no codec configuration"sv);

    auto selection = select_decoder_for_frame(frame, m_decoder_that_failed_due_to_missing_features);
    m_decoder_that_failed_due_to_missing_features = {};
    if (!selection.has_value())
        return DecoderError::format(DecoderErrorCategory::NotImplemented, "Could not find a video decoder for codec {}", frame.codec_id());

    m_decoder = TRY(create_video_decoder(selection, frame.codec_id(), *codec_initialization_data));
    m_decoder_selection = selection;
    // Only the decode thread waits for storage, so a release on the decode thread itself never needs a wake:
    // its next attempt to take output comes after this release in program order.
    m_decoder->set_storage_freed_callback([wait_state = m_wait_state, decode_thread_id = m_decode_thread_id] {
        if (decode_thread_id.is_current_thread())
            return;
        MutexLocker locker { wait_state->mutex };
        wait_state->frame_storage_was_freed = true;
        wait_state->condition.broadcast();
    });
    // Output that arrives while the decode thread reads coded data is taken as soon as the read returns.
    m_decoder->set_output_arrived_callback([wait_state = m_wait_state, demuxer = m_demuxer, track = m_track] {
        if (wait_state->output_should_interrupt_demuxer_reads.load(MemoryOrder::memory_order_relaxed))
            demuxer->set_blocking_reads_aborted_for_track(track);
    });
    m_decoder_codec_id = frame.codec_id();
    return {};
}

bool DecodedVideoProducer::ThreadData::check_if_seek_is_needed_or_allow_demuxer_reads(u32 seek_id)
{
    auto locker = take_lock();
    if (m_seek_id != seek_id)
        return true;
    m_demuxer->reset_blocking_reads_aborted_for_track(m_track);
    m_wait_state->output_should_interrupt_demuxer_reads.store(true, MemoryOrder::memory_order_relaxed);
    return false;
}

DecoderErrorOr<void> DecodedVideoProducer::ThreadData::receive_coded_frame(CodedFrame const& frame, DecodeIntent intent)
{
    VERIFY(!m_frame_awaiting_decoder_replacement.has_value());
    if (m_decoder == nullptr) [[unlikely]] {
        TRY(create_decoder_for_frame(frame));
    } else {
        auto frame_carries_new_config = frame.new_codec_configuration().has_value();
        auto decoder_needs_replacement = m_decoder_codec_id != frame.codec_id();
        if (!decoder_needs_replacement && frame_carries_new_config) {
            auto selection = select_decoder_for_frame(frame);
            decoder_needs_replacement = selection != m_decoder_selection;
        }
        if (decoder_needs_replacement) [[unlikely]] {
            replace_decoder_once_drained(frame, intent);
            return {};
        }
    }
    return receive_into_decoder(frame, intent);
}

DecoderErrorOr<bool> DecodedVideoProducer::ThreadData::replace_drained_decoder()
{
    if (!m_frame_awaiting_decoder_replacement.has_value())
        return false;
    auto frame = m_frame_awaiting_decoder_replacement.release_value();
    auto intent = m_intent_awaiting_decoder_replacement;
    m_decoder.clear();
    if (!frame.new_codec_configuration().has_value())
        return DecoderError::with_description(DecoderErrorCategory::NeedsResubmission, "The frame awaiting a new decoder carries no codec configuration"sv);
    TRY(create_decoder_for_frame(frame));
    TRY(receive_into_decoder(frame, intent));
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

DecodedVideoProducer::ThreadData::WaitResult DecodedVideoProducer::ThreadData::wait_for_frame_storage_while_locked()
{
    if (m_requested_state != RequestedState::Running)
        return WaitResult::ExitDecodeLoop;
    if (m_auto_suspend_requested)
        return WaitResult::ExitDecodeLoop;

    auto idle_at = m_last_consumer_activity + m_auto_suspend_idle_timeout;
    auto now = MonotonicTime::now();
    if (now < idle_at) {
        if (m_wait_state->condition.wait_for(idle_at - now))
            return WaitResult::ContinueDecoding;
        if (MonotonicTime::now() < idle_at)
            return WaitResult::ContinueDecoding;
    }

    m_auto_suspend_requested = true;
    return WaitResult::ExitDecodeLoop;
}

DecodedVideoProducer::FrameQueue& DecodedVideoProducer::ThreadData::queue()
{
    return m_queue;
}

void DecodedVideoProducer::ThreadData::seek(AK::Duration timestamp, InvalidateHeldData invalidate_held_data)
{
    auto locker = take_lock();
    VERIFY(!m_decode_thread_id.is_current_thread());
    note_consumer_activity_while_locked();
    m_downstream_needs_wake = true;

    auto seek_behavior = m_seek_reason.load(MemoryOrder::memory_order_relaxed);

    if (invalidate_held_data == InvalidateHeldData::Yes) {
        seek_behavior = SeekReason::DataInvalidated;
        m_seek_reason.store(seek_behavior, MemoryOrder::memory_order_relaxed);
    }

    if (seek_behavior != SeekReason::DataInvalidated && is_within_available_range_while_locked(timestamp)) {
        re_emit_last_frame_if_at_end_of_stream_while_locked();
        if (m_last_processed_seek_id != m_seek_id)
            resolve_seek(m_seek_id.load());
        dispatch_wake_if_needed_while_locked();
        return;
    }

    m_seek_id++;
    m_seek_timestamp = timestamp;
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

    m_queue.clear();
    m_last_queued_frame = nullptr;
    m_latest_available_timestamp = m_earliest_available_timestamp;
    m_decoder.clear();
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
    m_current_halting_status = PipelineStatus::Pending;

    return true;
}

void DecodedVideoProducer::ThreadData::queue_frame(NonnullRefPtr<VideoFrame> const& frame)
{
    m_queue.enqueue({ frame });
    m_last_queued_frame = frame;
    m_latest_available_timestamp = max(m_latest_available_timestamp, frame->conservative_end());
    dispatch_wake_if_needed_while_locked();
}

void DecodedVideoProducer::ThreadData::dispatch_error(DecoderError&& error)
{
    if (m_error_handler)
        m_error_handler(move(error));
}

bool DecodedVideoProducer::ThreadData::is_within_available_range_while_locked(AK::Duration timestamp) const
{
    return timestamp >= m_earliest_available_timestamp
        && (timestamp < m_latest_available_timestamp || m_current_halting_status == PipelineStatus::EndOfStream);
}

void DecodedVideoProducer::ThreadData::re_emit_last_frame_if_at_end_of_stream_while_locked()
{
    if (m_current_halting_status != PipelineStatus::EndOfStream || !m_queue.is_empty() || m_last_queued_frame == nullptr)
        return;
    queue_frame(*m_last_queued_frame);
}

void DecodedVideoProducer::ThreadData::resolve_seek(u32 seek_id)
{
    m_last_processed_seek_id = seek_id;
}

bool DecodedVideoProducer::ThreadData::seek_to_resubmit_if_decoder_lost_its_state(DecoderError const& error)
{
    if (error.category() != DecoderErrorCategory::NeedsResubmission)
        return false;

    auto locker = take_lock();
    seek_to_resubmit_while_locked();
    return true;
}

bool DecodedVideoProducer::ThreadData::seek_to_replace_decoder_if_it_cannot_decode_the_stream(DecoderError const& error)
{
    if (error.category() != DecoderErrorCategory::NotImplemented)
        return false;

    auto locker = take_lock();
    m_decoder_that_failed_due_to_missing_features = m_decoder_selection;
    m_decoder.clear();
    seek_to_resubmit_while_locked();
    return true;
}

void DecodedVideoProducer::ThreadData::seek_to_resubmit_while_locked()
{
    if (m_seek_reason.load(MemoryOrder::memory_order_relaxed) == SeekReason::Normal)
        m_seek_reason.store(SeekReason::DecoderResetNeeded, MemoryOrder::memory_order_relaxed);
    auto seek_is_pending = m_last_processed_seek_id != m_seek_id;
    m_seek_id++;
    // Interrupt the decode loop without hiding the held frames from peek().
    if (!seek_is_pending)
        m_last_processed_seek_id = m_seek_id.load();
    m_downstream_needs_wake = true;
}

void DecodedVideoProducer::ThreadData::handle_seek()
{
    VERIFY(m_decode_thread_id.is_current_thread());

    auto seek_id = m_seek_id.load(MemoryOrder::memory_order_relaxed);
    auto seek_was_externally_requested = seek_id != m_last_processed_seek_id.load(MemoryOrder::memory_order_relaxed);
    auto seek_reason = m_seek_reason.load(MemoryOrder::memory_order_relaxed);
    if (!seek_was_externally_requested && seek_reason != SeekReason::DecoderResetNeeded)
        return;

    AK::Duration timestamp;
    RefPtr<VideoFrame> last_frame;

    auto resolve_handled_seek_while_locked = [&] {
        resolve_seek(seek_id);
        if (m_seek_id == seek_id)
            m_seek_reason.store(SeekReason::Normal, MemoryOrder::memory_order_relaxed);
    };

    auto handle_error = [&](DecoderError&& error) {
        auto locker = take_lock();
        m_decoder_needs_keyframe_next_seek = true;
        enter_halting_state(PipelineStatus::Error, move(error));
        resolve_handled_seek_while_locked();
    };

    auto has_clamped_target = false;
    while (true) {
        {
            auto locker = take_lock();
            seek_id = m_seek_id.load(MemoryOrder::memory_order_relaxed);
            seek_was_externally_requested = seek_id != m_last_processed_seek_id;
            seek_reason = m_seek_reason.load(MemoryOrder::memory_order_relaxed);

            timestamp = m_seek_timestamp;
            m_demuxer->reset_blocking_reads_aborted_for_track(m_track);
            m_wait_state->output_should_interrupt_demuxer_reads.store(false, MemoryOrder::memory_order_relaxed);

            if (seek_reason == SeekReason::Normal) {
                if (is_within_available_range_while_locked(timestamp)) {
                    re_emit_last_frame_if_at_end_of_stream_while_locked();
                    resolve_handled_seek_while_locked();
                    dispatch_wake_if_needed_while_locked();
                    return;
                }
            } else {
                m_decoder_needs_keyframe_next_seek = true;
            }

            if (seek_reason == SeekReason::DecoderResetNeeded && !seek_was_externally_requested && !has_clamped_target) {
                timestamp = m_latest_available_timestamp;
                if (m_last_queued_frame != nullptr)
                    timestamp = m_last_queued_frame->end();
            } else {
                // If the demuxer keeps its position, the decoder won't output this frame again.
                if (!m_decoder_needs_keyframe_next_seek && m_last_queued_frame != nullptr && m_last_queued_frame->timestamp() <= timestamp)
                    last_frame = m_last_queued_frame;

                m_queue.clear();
                m_last_queued_frame = nullptr;
                m_earliest_available_timestamp = timestamp;
                m_latest_available_timestamp = timestamp;
            }

            m_current_halting_status = PipelineStatus::Pending;
            m_frame_awaiting_decoder_replacement.clear();
            if (m_decoder != nullptr)
                m_decoder_that_failed_due_to_missing_features = {};
        }

        auto seek_options = DemuxerSeekOptions::None;
        if (m_decoder == nullptr)
            seek_options |= DemuxerSeekOptions::NeedCodecConfiguration | DemuxerSeekOptions::Force;
        if (m_decoder_needs_keyframe_next_seek)
            seek_options |= DemuxerSeekOptions::Force;
        auto demuxer_seek_result_or_error = m_demuxer->seek_to_most_recent_keyframe(m_track, timestamp, seek_options);
        if (demuxer_seek_result_or_error.is_error() && demuxer_seek_result_or_error.error().category() != DecoderErrorCategory::EndOfStream) {
            if (demuxer_seek_result_or_error.error().category() == DecoderErrorCategory::Aborted)
                continue;
            handle_error(demuxer_seek_result_or_error.release_error());
            return;
        }

        m_decoder_needs_keyframe_next_seek = false;

        auto demuxer_seek_result = demuxer_seek_result_or_error.value_or(DemuxerSeekResult::MovedPosition);

        if (demuxer_seek_result == DemuxerSeekResult::MovedPosition) {
            if (m_decoder != nullptr)
                m_decoder->flush();
            last_frame.clear();
        }

        auto new_seek_id = m_seek_id.load();
        auto decoding_for_output = false;
        auto greatest_coded_timestamp = AK::Duration::zero();

        while (new_seek_id == seek_id) {
            if (check_if_seek_is_needed_or_allow_demuxer_reads(seek_id))
                break;

            while (m_decoder != nullptr && new_seek_id == seek_id) {
                auto frame_result = m_decoder->take_next_output(m_track.video_data().cicp, timestamp);
                if (frame_result.is_error()) {
                    if (frame_result.error().category() == DecoderErrorCategory::EndOfStream) {
                        auto error_or_decoder_was_replaced = replace_drained_decoder();
                        if (error_or_decoder_was_replaced.is_error()) {
                            if (seek_to_resubmit_if_decoder_lost_its_state(error_or_decoder_was_replaced.error())) {
                                new_seek_id = m_seek_id;
                                continue;
                            }
                            handle_error(error_or_decoder_was_replaced.release_error());
                            return;
                        }
                        if (error_or_decoder_was_replaced.value())
                            continue;

                        // If we've reached EOS without producing any output, restart the seek targeting the last frame
                        // we saw to produce just that last frame.
                        if (!has_clamped_target && !decoding_for_output && greatest_coded_timestamp > AK::Duration::zero()) {
                            auto locker = take_lock();
                            if (m_seek_id == seek_id) {
                                has_clamped_target = true;
                                m_seek_timestamp = greatest_coded_timestamp;
                                m_seek_id++;
                                m_decoder_needs_keyframe_next_seek = true;
                                new_seek_id = m_seek_id;
                                break;
                            }
                        }

                        auto locker = take_lock();
                        resolve_handled_seek_while_locked();
                        if (last_frame != nullptr)
                            queue_frame(last_frame.release_nonnull());
                        return;
                    }

                    if (frame_result.error().category() == DecoderErrorCategory::NeedsMoreInput)
                        break;

                    if (frame_result.error().category() == DecoderErrorCategory::TryAgain) {
                        auto locker = take_lock();
                        if (should_thread_exit_while_locked())
                            return;
                        if (m_seek_id != seek_id) {
                            new_seek_id = m_seek_id;
                            continue;
                        }
                        if (!m_wait_state->frame_storage_was_freed)
                            m_wait_state->condition.wait();
                        m_wait_state->frame_storage_was_freed = false;
                        continue;
                    }

                    if (seek_to_resubmit_if_decoder_lost_its_state(frame_result.error())) {
                        new_seek_id = m_seek_id;
                        continue;
                    }
                    if (seek_to_replace_decoder_if_it_cannot_decode_the_stream(frame_result.error())) {
                        new_seek_id = m_seek_id;
                        continue;
                    }
                    handle_error(frame_result.release_error());
                    return;
                }

                auto current_frame = frame_result.release_value();

                if (current_frame->contains_timestamp(timestamp)) {
                    auto locker = take_lock();
                    resolve_handled_seek_while_locked();
                    queue_frame(current_frame);
                    return;
                }

                if (current_frame->timestamp() > timestamp) {
                    auto locker = take_lock();
                    resolve_handled_seek_while_locked();

                    if (last_frame != nullptr)
                        queue_frame(last_frame.release_nonnull());

                    queue_frame(current_frame);
                    return;
                }

                last_frame = move(current_frame);

                new_seek_id = m_seek_id;
            }
            if (new_seek_id != seek_id)
                break;

            auto coded_frame_result = m_demuxer->get_next_sample_for_track(m_track);
            m_wait_state->output_should_interrupt_demuxer_reads.store(false, MemoryOrder::memory_order_relaxed);
            if (coded_frame_result.is_error()) {
                auto category = coded_frame_result.error().category();
                if (category == DecoderErrorCategory::EndOfStream) {
                    if (m_decoder == nullptr) {
                        auto locker = take_lock();
                        resolve_handled_seek_while_locked();
                        return;
                    }
                    m_decoder->signal_end_of_stream();
                } else if (category != DecoderErrorCategory::Aborted) {
                    handle_error(coded_frame_result.release_error());
                    return;
                }
                continue;
            }

            auto coded_frame = coded_frame_result.release_value();

            // Request output for any frames that end after the seek target, indicating that they will need to
            // be displayed (eventually, in reordered codecs).
            auto conservative_end_of_coded_frame = VideoFrame::conservative_end_of(coded_frame.presentation_timestamp(), coded_frame.duration());
            if (conservative_end_of_coded_frame >= timestamp)
                decoding_for_output = true;
            greatest_coded_timestamp = max(greatest_coded_timestamp, coded_frame.presentation_timestamp());

            auto decode_result = receive_coded_frame(coded_frame, decoding_for_output ? DecodeIntent::Output : DecodeIntent::Reference);
            if (decode_result.is_error()) {
                if (seek_to_resubmit_if_decoder_lost_its_state(decode_result.error())) {
                    new_seek_id = m_seek_id;
                    continue;
                }
                handle_error(decode_result.release_error());
                return;
            }

            new_seek_id = m_seek_id;
        }
    }
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

    if (check_if_seek_is_needed_or_allow_demuxer_reads(m_last_processed_seek_id.load(MemoryOrder::memory_order_relaxed)))
        return;

    auto seek_is_pending = [&] {
        return m_seek_id.load(MemoryOrder::memory_order_relaxed) != m_last_processed_seek_id.load(MemoryOrder::memory_order_relaxed);
    };

    while (m_decoder != nullptr) {
        if (seek_is_pending())
            return;

        auto frame_result = m_decoder->take_next_output(m_track.video_data().cicp);
        if (frame_result.is_error()) {
            if (frame_result.error().category() == DecoderErrorCategory::NeedsMoreInput)
                break;
            if (frame_result.error().category() == DecoderErrorCategory::TryAgain) {
                auto locker = take_lock();
                if (seek_is_pending())
                    return;
                if (!m_wait_state->frame_storage_was_freed && wait_for_frame_storage_while_locked() == WaitResult::ExitDecodeLoop)
                    return;
                m_wait_state->frame_storage_was_freed = false;
                if (should_thread_exit_while_locked())
                    return;
                continue;
            }
            if (seek_to_resubmit_if_decoder_lost_its_state(frame_result.error()))
                return;
            if (seek_to_replace_decoder_if_it_cannot_decode_the_stream(frame_result.error()))
                return;
            if (frame_result.error().category() == DecoderErrorCategory::EndOfStream) {
                auto replaced_result = replace_drained_decoder();
                if (replaced_result.is_error()) {
                    if (seek_to_resubmit_if_decoder_lost_its_state(replaced_result.error()))
                        return;
                    set_halting_status_and_wait_for_seek(PipelineStatus::Error, replaced_result.release_error());
                    return;
                }
                if (replaced_result.value())
                    continue;
                set_halting_status_and_wait_for_seek(PipelineStatus::EndOfStream, {});
                return;
            }
            set_halting_status_and_wait_for_seek(PipelineStatus::Error, frame_result.release_error());
            return;
        }
        auto frame = frame_result.release_value();

        auto locker = take_lock();
        queue_frame(frame);
    }

    // A seek can resolve while the decoder drains for a replacement, so the frame awaiting the new decoder is still
    // pending. Only draining further gets it decoded.
    if (m_frame_awaiting_decoder_replacement.has_value())
        return;

    if (seek_is_pending())
        return;

    auto sample_result = m_demuxer->get_next_sample_for_track(m_track);
    m_wait_state->output_should_interrupt_demuxer_reads.store(false, MemoryOrder::memory_order_relaxed);
    if (sample_result.is_error()) {
        auto category = sample_result.error().category();
        if (category == DecoderErrorCategory::EndOfStream) {
            if (m_decoder == nullptr) {
                set_halting_status_and_wait_for_seek(PipelineStatus::EndOfStream, {});
                return;
            }
            m_decoder->signal_end_of_stream();
        } else if (category != DecoderErrorCategory::Aborted) {
            set_halting_status_and_wait_for_seek(PipelineStatus::Error, sample_result.release_error());
        }
        return;
    }

    auto coded_frame = sample_result.release_value();
    auto decode_result = receive_coded_frame(coded_frame, DecodeIntent::Output);
    if (decode_result.is_error()) {
        if (seek_to_resubmit_if_decoder_lost_its_state(decode_result.error()))
            return;
        set_halting_status_and_wait_for_seek(PipelineStatus::Error, decode_result.release_error());
    }
}

}
