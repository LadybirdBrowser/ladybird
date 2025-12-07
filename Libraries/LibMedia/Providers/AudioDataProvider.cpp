/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <LibCore/EventLoop.h>
#include <LibMedia/FFmpeg/FFmpegAudioDecoder.h>
#include <LibMedia/MutexedDemuxer.h>
#include <LibMedia/Sinks/AudioSink.h>
#include <LibThreading/Mutex.h>
#include <LibThreading/Thread.h>

#include "AudioDataProvider.h"

namespace Media {

DecoderErrorOr<NonnullRefPtr<AudioDataProvider>> AudioDataProvider::try_create(Core::EventLoop& main_thread_event_loop, NonnullRefPtr<MutexedDemuxer> const& demuxer, Track const& track)
{
    auto codec_id = TRY(demuxer->get_codec_id_for_track(track));
    auto codec_initialization_data = TRY(demuxer->get_codec_initialization_data_for_track(track));
    auto decoder = DECODER_TRY_ALLOC(FFmpeg::FFmpegAudioDecoder::try_create(codec_id, codec_initialization_data));

    auto thread_data = DECODER_TRY_ALLOC(try_make_ref_counted<AudioDataProvider::ThreadData>(main_thread_event_loop, demuxer, track, move(decoder)));
    auto provider = DECODER_TRY_ALLOC(try_make_ref_counted<AudioDataProvider>(thread_data));

    auto thread = DECODER_TRY_ALLOC(Threading::Thread::try_create([thread_data]() -> int {
        thread_data->wait_for_start();
        while (!thread_data->should_thread_exit()) {
            thread_data->handle_seek();
            thread_data->push_data_and_decode_a_block();
        }
        return 0;
    }));
    thread->start();
    thread->detach();

    return provider;
}

AudioDataProvider::AudioDataProvider(NonnullRefPtr<ThreadData> const& thread_data)
    : m_thread_data(thread_data)
{
}

AudioDataProvider::~AudioDataProvider()
{
    m_thread_data->exit();
}

void AudioDataProvider::set_error_handler(ErrorHandler&& handler)
{
    m_thread_data->set_error_handler(move(handler));
}

void AudioDataProvider::start()
{
    m_thread_data->start();
}

void AudioDataProvider::seek(AK::Duration timestamp, SeekCompletionHandler&& completion_handler)
{
    m_thread_data->seek(timestamp, move(completion_handler));
}

AudioDataProvider::ThreadData::ThreadData(Core::EventLoop& main_thread_event_loop, NonnullRefPtr<MutexedDemuxer> const& demuxer, Track const& track, NonnullOwnPtr<AudioDecoder>&& decoder)
    : m_main_thread_event_loop(main_thread_event_loop)
    , m_demuxer(demuxer)
    , m_track(track)
    , m_decoder(move(decoder))
{
}

AudioDataProvider::ThreadData::~ThreadData() = default;

void AudioDataProvider::ThreadData::set_error_handler(ErrorHandler&& handler)
{
    m_error_handler = move(handler);
}

void AudioDataProvider::ThreadData::start()
{
    auto locker = take_lock();
    VERIFY(m_requested_state == RequestedState::None);
    m_requested_state = RequestedState::Running;
    wake();
}

void AudioDataProvider::ThreadData::exit()
{
    auto locker = take_lock();
    m_requested_state = RequestedState::Exit;
    wake();
}

AudioBlock AudioDataProvider::retrieve_block()
{
    auto locker = m_thread_data->take_lock();
    if (m_thread_data->queue().is_empty())
        return AudioBlock();
    auto result = m_thread_data->queue().dequeue();
    m_thread_data->wake();
    return result;
}

void AudioDataProvider::ThreadData::seek(AK::Duration timestamp, SeekCompletionHandler&& completion_handler)
{
    auto locker = take_lock();
    m_seek_completion_handler = move(completion_handler);
    m_seek_id++;
    m_seek_timestamp = timestamp;
    wake();
}

void AudioDataProvider::ThreadData::wait_for_start()
{
    auto locker = take_lock();
    while (m_requested_state == RequestedState::None)
        m_wait_condition.wait();
}

bool AudioDataProvider::ThreadData::should_thread_exit() const
{
    auto locker = take_lock();
    return m_requested_state == RequestedState::Exit;
}

void AudioDataProvider::ThreadData::flush_decoder()
{
    m_decoder->flush();
    m_last_sample = NumericLimits<i64>::min();
}

DecoderErrorOr<void> AudioDataProvider::ThreadData::retrieve_next_block(AudioBlock& block)
{
    TRY(m_decoder->write_next_block(block));
    if (block.timestamp_in_samples() < m_last_sample)
        block.set_timestamp_in_samples(m_last_sample);
    m_last_sample = block.timestamp_in_samples() + static_cast<i64>(block.sample_count());
    return {};
}

template<typename T>
void AudioDataProvider::ThreadData::process_seek_on_main_thread(u32 seek_id, T&& function)
{
    m_last_processed_seek_id = seek_id;
    m_main_thread_event_loop.deferred_invoke([self = NonnullRefPtr(*this), seek_id, function] mutable {
        if (self->m_seek_id != seek_id)
            return;
        function();
    });
}

void AudioDataProvider::ThreadData::resolve_seek(u32 seek_id)
{
    m_is_in_error_state = false;
    process_seek_on_main_thread(seek_id, [self = NonnullRefPtr(*this)] {
        auto handler = move(self->m_seek_completion_handler);
        if (handler)
            handler();
    });
}

bool AudioDataProvider::ThreadData::handle_seek()
{
    auto seek_id = m_seek_id.load();
    if (m_last_processed_seek_id == seek_id)
        return false;

    auto handle_error = [&](DecoderError&& error) {
        m_is_in_error_state = true;
        {
            auto locker = take_lock();
            m_queue.clear();
        }
        process_seek_on_main_thread(seek_id,
            [self = NonnullRefPtr(*this), error = move(error)] mutable {
                if (self->m_error_handler)
                    self->m_error_handler(move(error));
                self->m_seek_completion_handler = nullptr;
            });
    };

    AK::Duration timestamp;

    while (true) {
        {
            auto locker = take_lock();
            seek_id = m_seek_id;
            timestamp = m_seek_timestamp;
        }

        auto demuxer_seek_result_or_error = m_demuxer->seek_to_most_recent_keyframe(m_track, timestamp);
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
                    m_queue.clear();

                    if (!last_block.is_empty())
                        m_queue.enqueue(move(last_block));

                    m_queue.enqueue(move(current_block));

                    resolve_seek(seek_id);
                    return true;
                }

                last_block = move(current_block);

                new_seek_id = m_seek_id;
            }
        }
    }
}

void AudioDataProvider::ThreadData::push_data_and_decode_a_block()
{
    auto set_error_and_wait_for_seek = [this](DecoderError&& error) {
        {
            auto locker = take_lock();
            m_is_in_error_state = true;
            m_main_thread_event_loop.deferred_invoke([self = NonnullRefPtr(*this), error = move(error)] mutable {
                if (self->m_error_handler)
                    self->m_error_handler(move(error));
            });
        }

        dbgln_if(PLAYBACK_MANAGER_DEBUG, "Audio Data Provider: Encountered an error, waiting for a seek to start decoding again...");
        while (m_is_in_error_state) {
            if (handle_seek())
                break;
            {
                auto locker = take_lock();
                m_wait_condition.wait();
            }
        }
    };

    auto sample_result = m_demuxer->get_next_sample_for_track(m_track);
    if (sample_result.is_error()) {
        if (sample_result.error().category() == DecoderErrorCategory::EndOfStream) {
            m_decoder->signal_end_of_stream();
        } else {
            set_error_and_wait_for_seek(sample_result.release_error());
            return;
        }
    } else {
        auto sample = sample_result.release_value();
        auto decode_result = m_decoder->receive_coded_data(sample.timestamp(), sample.data());
        if (decode_result.is_error()) {
            set_error_and_wait_for_seek(decode_result.release_error());
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

            {
                auto locker = take_lock();
                m_wait_condition.wait();
                if (should_thread_exit())
                    return;
                queue_size = m_queue.size();
            }
        }

        auto block = AudioBlock();
        auto timestamp_result = retrieve_next_block(block);
        if (timestamp_result.is_error()) {
            if (timestamp_result.error().category() == DecoderErrorCategory::NeedsMoreInput)
                break;
            set_error_and_wait_for_seek(timestamp_result.release_error());
            break;
        }

        // FIXME: Specify trailing samples in the demuxer, and drop them here or in the audio decoder implementation.

        auto locker = take_lock();
        VERIFY(!block.is_empty());
        m_queue.enqueue(move(block));
        VERIFY(!m_queue.tail().is_empty());
    }
}

}
