/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <LibCore/EventLoop.h>
#include <LibMedia/Audio/SampleSpecification.h>
#include <LibMedia/Demuxer.h>
#include <LibMedia/FFmpeg/FFmpegAudioConverter.h>
#include <LibMedia/FFmpeg/FFmpegAudioDecoder.h>
#include <LibMedia/Sinks/AudioSink.h>
#include <LibSync/Mutex.h>
#include <LibThreading/Thread.h>

#include "DecodedAudioProducer.h"

namespace Media {

DecoderErrorOr<NonnullRefPtr<DecodedAudioProducer>> DecodedAudioProducer::try_create(NonnullRefPtr<Core::WeakEventLoopReference> const& main_thread_event_loop, NonnullRefPtr<Demuxer> const& demuxer, Track const& track)
{
    auto converter = DECODER_TRY_ALLOC(FFmpeg::FFmpegAudioConverter::try_create());

    TRY(demuxer->create_context_for_track(track));
    auto duration = TRY(demuxer->duration_of_track(track));
    auto thread_data = DECODER_TRY_ALLOC(try_make_ref_counted<DecodedAudioProducer::ThreadData>(main_thread_event_loop, demuxer, track, duration, move(converter)));
    TRY(thread_data->create_decoder());
    auto producer = DECODER_TRY_ALLOC(try_make_ref_counted<DecodedAudioProducer>(thread_data));

    auto thread = DECODER_TRY_ALLOC(Threading::Thread::try_create("Audio Decoder"sv, [thread_data]() -> int {
        thread_data->wait_for_start();
        while (!thread_data->should_thread_exit()) {
            if (thread_data->handle_suspension())
                continue;
            thread_data->handle_seek();
            thread_data->push_data_and_decode_a_block();
        }
        return 0;
    }));
    thread->start();
    thread->detach();

    return producer;
}

DecodedAudioProducer::DecodedAudioProducer(NonnullRefPtr<ThreadData> const& thread_data)
    : m_thread_data(thread_data)
{
}

DecodedAudioProducer::~DecodedAudioProducer()
{
    m_thread_data->exit();
}

void DecodedAudioProducer::set_error_handler(ErrorHandler&& handler)
{
    m_thread_data->set_error_handler(move(handler));
}

void DecodedAudioProducer::set_duration_change_handler(BlockEndTimeHandler&& handler)
{
    m_thread_data->set_duration_change_handler(move(handler));
}

void DecodedAudioProducer::set_output_sample_specification(Audio::SampleSpecification sample_specification)
{
    m_thread_data->set_output_sample_specification(sample_specification);
}

void DecodedAudioProducer::start()
{
    m_thread_data->start();
}

void DecodedAudioProducer::suspend()
{
    m_thread_data->suspend();
}

void DecodedAudioProducer::resume()
{
    m_thread_data->resume();
}

void DecodedAudioProducer::seek(AK::Duration timestamp, SeekCompletionHandler&& completion_handler)
{
    m_thread_data->seek(timestamp, move(completion_handler));
}

TimeRanges DecodedAudioProducer::buffered_time_ranges() const
{
    return m_thread_data->buffered_time_ranges();
}

DecodedAudioProducer::ThreadData::ThreadData(NonnullRefPtr<Core::WeakEventLoopReference> const& main_thread_event_loop, NonnullRefPtr<Demuxer> const& demuxer, Track const& track, AK::Duration duration, NonnullOwnPtr<Audio::AudioConverter>&& converter)
    : m_main_thread_event_loop(main_thread_event_loop)
    , m_demuxer(demuxer)
    , m_track(track)
    , m_duration(duration)
    , m_converter(move(converter))
{
}

DecodedAudioProducer::ThreadData::~ThreadData() = default;

void DecodedAudioProducer::ThreadData::set_error_handler(ErrorHandler&& handler)
{
    m_error_handler = move(handler);
}

void DecodedAudioProducer::ThreadData::set_duration_change_handler(BlockEndTimeHandler&& handler)
{
    m_duration_change_handler = move(handler);
}

void DecodedAudioProducer::ThreadData::set_output_sample_specification(Audio::SampleSpecification sample_specification)
{
    m_converter->set_output_sample_specification(sample_specification).release_value_but_fixme_should_propagate_errors();
}

void DecodedAudioProducer::ThreadData::start()
{
    auto locker = take_lock();
    if (m_requested_state != RequestedState::None)
        return;
    m_requested_state = RequestedState::Running;
    wake();
}

DecoderErrorOr<void> DecodedAudioProducer::ThreadData::create_decoder()
{
    auto codec_id = TRY(m_demuxer->get_codec_id_for_track(m_track));
    auto const& sample_specification = m_track.audio_data().sample_specification;
    auto codec_initialization_data = TRY(m_demuxer->get_codec_initialization_data_for_track(m_track));
    m_decoder = TRY(FFmpeg::FFmpegAudioDecoder::try_create(codec_id, sample_specification, codec_initialization_data));
    return {};
}

void DecodedAudioProducer::ThreadData::suspend()
{
    auto locker = take_lock();
    VERIFY(m_requested_state != RequestedState::Exit);
    m_requested_state = RequestedState::Suspended;
    wake();
}

void DecodedAudioProducer::ThreadData::resume()
{
    auto locker = take_lock();
    VERIFY(m_requested_state != RequestedState::Exit);
    m_requested_state = RequestedState::Running;
    wake();
}

void DecodedAudioProducer::ThreadData::exit()
{
    auto locker = take_lock();
    m_requested_state = RequestedState::Exit;
    wake();
}

PipelineStatus DecodedAudioProducer::pull(AudioBlock& into)
{
    return m_thread_data->pull(into);
}

PipelineStatus DecodedAudioProducer::ThreadData::pull(AudioBlock& into)
{
    auto locker = take_lock();
    if (!m_queue.is_empty()) {
        into = m_queue.dequeue();
        wake();
        return PipelineStatus::HaveData;
    }
    if (m_pending_halting_status != PipelineStatus::Pending)
        return m_pending_halting_status;
    if (m_demuxer->is_read_blocked_for_track(m_track))
        return PipelineStatus::Blocked;
    return PipelineStatus::Pending;
}

void DecodedAudioProducer::ThreadData::enter_halting_state(PipelineStatus status, Optional<DecoderError> error)
{
    if (error.has_value() && error->category() == DecoderErrorCategory::Aborted)
        return;

    VERIFY(status == PipelineStatus::EndOfStream || status == PipelineStatus::Error);
    m_pending_halting_status = status;
    if (error.has_value()) {
        invoke_on_main_thread_while_locked([error = error.release_value()](auto const& self) mutable {
            self->dispatch_error(move(error));
        });
    }
}

void DecodedAudioProducer::ThreadData::seek(AK::Duration timestamp, SeekCompletionHandler&& completion_handler)
{
    auto locker = take_lock();
    m_seek_completion_handler = move(completion_handler);
    m_seek_id++;
    m_seek_timestamp = timestamp;
    m_demuxer->set_blocking_reads_aborted_for_track(m_track);
    wake();
}

void DecodedAudioProducer::ThreadData::wait_for_start()
{
    auto locker = take_lock();
    while (m_requested_state == RequestedState::None)
        m_wait_condition.wait();
}

bool DecodedAudioProducer::ThreadData::should_thread_exit_while_locked() const
{
    return m_requested_state == RequestedState::Exit;
}

bool DecodedAudioProducer::ThreadData::should_thread_exit() const
{
    auto locker = take_lock();
    return should_thread_exit_while_locked();
}

bool DecodedAudioProducer::ThreadData::handle_suspension()
{
    {
        auto locker = take_lock();
        if (m_requested_state != RequestedState::Suspended)
            return false;

        clear_queue();
        m_decoder.clear();
        m_decoder_needs_keyframe_next_seek = true;

        while (m_requested_state == RequestedState::Suspended)
            m_wait_condition.wait();

        if (m_requested_state != RequestedState::Running)
            return true;

        auto result = create_decoder();
        if (result.is_error())
            enter_halting_state(PipelineStatus::Error, result.release_error());
    }

    // Suspension must be woken with a seek, or we will throw decoding errors.
    while (!handle_seek()) {
        auto locker = take_lock();
        m_wait_condition.wait();
        if (should_thread_exit_while_locked())
            return true;
    }

    return true;
}

template<typename Invokee>
void DecodedAudioProducer::ThreadData::invoke_on_main_thread_while_locked(Invokee invokee)
{
    if (m_requested_state == RequestedState::Exit)
        return;
    auto event_loop = m_main_thread_event_loop->take();
    if (!event_loop.is_alive())
        return;
    event_loop->deferred_invoke([self = NonnullRefPtr(*this), invokee = move(invokee)] mutable {
        invokee(self);
    });
}

template<typename Invokee>
void DecodedAudioProducer::ThreadData::invoke_on_main_thread(Invokee invokee)
{
    auto locker = take_lock();
    invoke_on_main_thread_while_locked(move(invokee));
}

void DecodedAudioProducer::ThreadData::dispatch_block_end_time(AudioBlock const& block)
{
    auto end_time = block.end_timestamp();
    if (end_time < m_duration)
        return;
    m_duration = end_time;
    invoke_on_main_thread_while_locked([end_time](auto const& self) {
        if (self->m_duration_change_handler)
            self->m_duration_change_handler(end_time);
    });
}

void DecodedAudioProducer::ThreadData::clear_queue()
{
    m_queue.clear();
}

void DecodedAudioProducer::ThreadData::queue_block(AudioBlock&& block)
{
    // FIXME: Specify trailing samples in the demuxer, and drop them here or in the audio decoder implementation.

    VERIFY(!block.is_empty());
    dispatch_block_end_time(block);
    m_queue.enqueue(move(block));
    VERIFY(!m_queue.tail().is_empty());
}

void DecodedAudioProducer::ThreadData::dispatch_error(DecoderError&& error)
{
    if (error.category() == DecoderErrorCategory::Aborted)
        return;
    if (m_error_handler)
        m_error_handler(move(error));
}

void DecodedAudioProducer::ThreadData::flush_decoder()
{
    m_decoder->flush();
    m_last_sample = NumericLimits<i64>::min();
}

DecoderErrorOr<void> DecodedAudioProducer::ThreadData::retrieve_next_block(AudioBlock& block)
{
    TRY(m_decoder->write_next_block(block));

    auto convert_result = m_converter->convert(block);
    if (convert_result.is_error())
        return DecoderError::format(DecoderErrorCategory::NotImplemented, "Sample specification conversion failed: {}", convert_result.error().string_literal());

    if (block.timestamp_in_samples() < m_last_sample)
        block.set_timestamp_in_samples(m_last_sample);
    m_last_sample = block.timestamp_in_samples() + static_cast<i64>(block.sample_count());
    return {};
}

template<typename Callback>
void DecodedAudioProducer::ThreadData::process_seek_on_main_thread(u32 seek_id, Callback callback)
{
    m_last_processed_seek_id = seek_id;
    invoke_on_main_thread_while_locked([seek_id, callback = move(callback)](auto& self) mutable {
        if (self->m_seek_id != seek_id)
            return;
        callback(self);
    });
}

void DecodedAudioProducer::ThreadData::resolve_seek(u32 seek_id)
{
    m_pending_halting_status = PipelineStatus::Pending;
    process_seek_on_main_thread(seek_id, [](auto& self) {
        auto handler = move(self->m_seek_completion_handler);
        if (handler)
            handler();
    });
}

bool DecodedAudioProducer::ThreadData::handle_seek()
{
    VERIFY(m_decoder);

    auto seek_id = m_seek_id.load();
    if (m_last_processed_seek_id == seek_id)
        return false;

    auto handle_error = [&](DecoderError&& error) {
        auto locker = take_lock();
        clear_queue();
        enter_halting_state(PipelineStatus::Error, move(error));
        process_seek_on_main_thread(seek_id, [](auto& self) {
            self->m_seek_completion_handler = nullptr;
        });
    };

    AK::Duration timestamp;

    while (true) {
        {
            auto locker = take_lock();
            seek_id = m_seek_id;
            timestamp = m_seek_timestamp;
            m_demuxer->reset_blocking_reads_aborted_for_track(m_track);
        }

        auto seek_options = DemuxerSeekOptions::None;
        if (m_decoder_needs_keyframe_next_seek) {
            seek_options |= DemuxerSeekOptions::Force;
            m_decoder_needs_keyframe_next_seek = false;
        }
        auto demuxer_seek_result_or_error = m_demuxer->seek_to_most_recent_keyframe(m_track, timestamp, seek_options);
        if (demuxer_seek_result_or_error.is_error() && demuxer_seek_result_or_error.error().category() != DecoderErrorCategory::EndOfStream) {
            handle_error(demuxer_seek_result_or_error.release_error());
            return true;
        }
        auto demuxer_seek_result = demuxer_seek_result_or_error.value_or(DemuxerSeekResult::MovedPosition);

        if (demuxer_seek_result == DemuxerSeekResult::MovedPosition)
            flush_decoder();

        auto new_seek_id = seek_id;
        AudioBlock last_block;

        while (new_seek_id == seek_id) {
            auto coded_frame_result = m_demuxer->get_next_sample_for_track(m_track);
            if (coded_frame_result.is_error()) {
                if (coded_frame_result.error().category() == DecoderErrorCategory::EndOfStream) {
                    m_decoder->signal_end_of_stream();
                } else {
                    handle_error(coded_frame_result.release_error());
                    return true;
                }
            } else {
                auto coded_frame = coded_frame_result.release_value();
                auto decode_result = m_decoder->receive_coded_data(coded_frame.timestamp(), coded_frame.data());
                if (decode_result.is_error()) {
                    handle_error(decode_result.release_error());
                    return true;
                }
            }

            while (new_seek_id == seek_id) {
                AudioBlock current_block;
                auto block_result = retrieve_next_block(current_block);
                if (block_result.is_error()) {
                    if (block_result.error().category() == DecoderErrorCategory::EndOfStream) {
                        auto locker = take_lock();
                        resolve_seek(seek_id);
                        return true;
                    }

                    if (block_result.error().category() == DecoderErrorCategory::NeedsMoreInput)
                        break;

                    handle_error(block_result.release_error());
                    return true;
                }

                if (current_block.timestamp() > timestamp) {
                    auto locker = take_lock();
                    clear_queue();

                    if (!last_block.is_empty())
                        queue_block(move(last_block));

                    queue_block(move(current_block));

                    resolve_seek(seek_id);
                    return true;
                }

                last_block = move(current_block);

                new_seek_id = m_seek_id;
            }
        }
    }
}

void DecodedAudioProducer::ThreadData::push_data_and_decode_a_block()
{
    VERIFY(m_decoder);

    auto set_halting_status_and_wait_for_seek = [this](PipelineStatus status, Optional<DecoderError> error) {
        {
            auto locker = take_lock();
            enter_halting_state(status, move(error));
        }

        dbgln_if(PLAYBACK_MANAGER_DEBUG, "Decoded Audio Producer: Reached a halting pull status, waiting for a seek to start decoding again...");
        while (true) {
            {
                auto locker = take_lock();
                if (m_pending_halting_status == PipelineStatus::Pending)
                    return;
            }
            if (handle_seek())
                break;
            {
                auto locker = take_lock();
                m_wait_condition.wait();
                if (should_thread_exit_while_locked())
                    return;
            }
        }
    };

    auto sample_result = m_demuxer->get_next_sample_for_track(m_track);
    if (sample_result.is_error()) {
        if (sample_result.error().category() == DecoderErrorCategory::EndOfStream) {
            m_decoder->signal_end_of_stream();
        } else {
            set_halting_status_and_wait_for_seek(PipelineStatus::Error, sample_result.release_error());
            return;
        }
    } else {
        auto sample = sample_result.release_value();
        auto decode_result = m_decoder->receive_coded_data(sample.timestamp(), sample.data());
        if (decode_result.is_error()) {
            set_halting_status_and_wait_for_seek(PipelineStatus::Error, decode_result.release_error());
            return;
        }
    }

    while (true) {
        auto queue_size = [&] {
            auto locker = take_lock();
            return m_queue.size();
        }();

        while (queue_size >= m_queue_max_size) {
            if (handle_seek())
                return;

            if (handle_suspension())
                return;

            {
                auto locker = take_lock();
                queue_size = m_queue.size();
                if (queue_size < m_queue_max_size)
                    continue;
                m_wait_condition.wait();
                if (should_thread_exit_while_locked())
                    return;
                queue_size = m_queue.size();
            }
        }

        auto block = AudioBlock();
        auto block_result = retrieve_next_block(block);
        if (block_result.is_error()) {
            if (block_result.error().category() == DecoderErrorCategory::NeedsMoreInput)
                break;
            if (block_result.error().category() == DecoderErrorCategory::EndOfStream)
                set_halting_status_and_wait_for_seek(PipelineStatus::EndOfStream, {});
            else
                set_halting_status_and_wait_for_seek(PipelineStatus::Error, block_result.release_error());
            break;
        }

        auto locker = take_lock();
        queue_block(move(block));
    }
}

TimeRanges DecodedAudioProducer::ThreadData::buffered_time_ranges() const
{
    return m_demuxer->buffered_time_ranges();
}

}
